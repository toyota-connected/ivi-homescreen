/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "backend/drm_kms_egl/drm_backend.h"
#include "display/drm_display.h"    // DrainReadyFlips from WaitForPendingFlip
#include "display/drm_mode_list.h"  // ConnectorTypeName, PrintDrmModes (shared)
#include "logging/logging.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <linux/vt.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <drm-cxx/core/format.hpp>
#include <drm-cxx/log.hpp>
#include "backend/drm_kms_egl/driver_probe.h"

#include "backend/drm_kms_egl/drm_capture.h"
#include "backend/drm_kms_egl/drm_compositor.h"
#include "backend/drm_kms_egl/drm_cursor.h"
#include "backend/drm_kms_egl/drm_session.h"
#include "backend/drm_kms_egl/gl_cursor.h"
#include "backend/gl_process_resolver.h"
#if BUILD_HUD
#include "backend/hud/gl_hud.h"
#endif
#include "engine.h"
#include "logging.h"
#include "profiling/frame_profile.h"
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"
#include "shell/platform/homescreen/flutter_desktop_view_controller_state.h"
#include "task_runner.h"

#if BUILD_COMPOSITOR
#include "view/compositor_surface_interface.h"
#endif

namespace {

// EGL attribute arrays are selected per resolved primary format. All
// variants request WINDOW_BIT + ES2. R/G/B/A and depth sizes differ for
// RGB565 (5/6/5/0, depth 16) vs. 32-bit formats (8/8/8/{0|8}, depth 24).
// ALPHA_SIZE=8 is preferred for ARGB/ABGR primaries, so Flutter's blend
// operations land in a format matching the scanout BO.

constexpr std::array<EGLint, 15> kEglAttrs_8880_D24 = {{
    EGL_SURFACE_TYPE,
    EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE,
    EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE,
    8,
    EGL_GREEN_SIZE,
    8,
    EGL_BLUE_SIZE,
    8,
    EGL_ALPHA_SIZE,
    0,
    EGL_DEPTH_SIZE,
    24,
    EGL_NONE,
}};
constexpr std::array<EGLint, 15> kEglAttrs_8888_D24 = {{
    EGL_SURFACE_TYPE,
    EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE,
    EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE,
    8,
    EGL_GREEN_SIZE,
    8,
    EGL_BLUE_SIZE,
    8,
    EGL_ALPHA_SIZE,
    8,
    EGL_DEPTH_SIZE,
    24,
    EGL_NONE,
}};
constexpr std::array<EGLint, 15> kEglAttrs_5650_D16 = {{
    EGL_SURFACE_TYPE,
    EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE,
    EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE,
    5,
    EGL_GREEN_SIZE,
    6,
    EGL_BLUE_SIZE,
    5,
    EGL_ALPHA_SIZE,
    0,
    EGL_DEPTH_SIZE,
    16,
    EGL_NONE,
}};

// Pick the EGL attribute array that matches the scanout primary format.
const EGLint* EglAttrsFor(const uint32_t fourcc) {
  switch (fourcc) {
    case GBM_FORMAT_ARGB8888:
    case GBM_FORMAT_ABGR8888:
      return kEglAttrs_8888_D24.data();
    case GBM_FORMAT_RGB565:
      return kEglAttrs_5650_D16.data();
    case GBM_FORMAT_XRGB8888:
    case GBM_FORMAT_XBGR8888:
    default:
      return kEglAttrs_8880_D24.data();
  }
}

// For scoring EGL configs against the resolved primary: XRGB/XBGR want
// alpha=0, ARGB/ABGR want alpha=8, RGB565 wants alpha=0.
EGLint PreferredAlphaFor(const uint32_t fourcc) {
  return (fourcc == GBM_FORMAT_ARGB8888 || fourcc == GBM_FORMAT_ABGR8888) ? 8
                                                                          : 0;
}

constexpr std::array<EGLint, 3> kEsContextAttribs = {{
    EGL_CONTEXT_CLIENT_VERSION,
    2,
    EGL_NONE,
}};

DrmBackend* BackendFromState(void* user_data) {
  const auto state = static_cast<FlutterDesktopEngineState*>(user_data);
  return reinterpret_cast<DrmBackend*>(
      state->view_controller->engine->GetBackend());
}

}  // namespace

std::unique_ptr<DrmBackend> DrmBackend::Create(const DrmConfig& cfg,
                                               homescreen::DrmSession* session,
                                               drm::Device* shared_device,
                                               DrmDisplay* display) {
  std::unique_ptr<DrmBackend> backend(new DrmBackend(cfg, session));
  backend->drm_dev_ = shared_device;
  backend->drm_display_ = display;
  if (!backend->InitDrm()) {
    return nullptr;
  }

  // Observability-only hotplug handler: log every connector plug/unplug
  // uevent so field traces show display changes. No state mutation yet.
  if (session != nullptr) {
    const uint32_t our_connector = backend->connector_id_;
    session->set_hotplug_handler(
        [our_connector](const drm::display::HotplugEvent& e) {
          const std::string id_str = e.connector_id
                                         ? std::to_string(*e.connector_id)
                                         : std::string("<unspecified>");
          const bool ours = e.connector_id && *e.connector_id == our_connector;
          ihs::log::info(
              "[DrmBackend] hotplug: devnode={} connector_id={}{}",
              e.devnode.empty() ? std::string_view{"<unknown>"} : e.devnode,
              id_str, ours ? " (active connector)" : "");
        });
  }

  // Resolve tristate knobs now — InitDrm has the CRTC selected and the
  // DRM caps set. The probe drives format choice in InitGbm below, so
  // this must happen before it.
  backend->resolved_ = std::make_unique<homescreen::driver_probe::Resolved>(
      homescreen::driver_probe::Resolve(backend->drm_dev_->fd(),
                                        backend->connector_id_,
                                        backend->crtc_id_, cfg));
  homescreen::driver_probe::LogResolved(*backend->resolved_);

  if (!backend->InitGbm() || !backend->InitEgl()) {
    return nullptr;
  }
#if BUILD_COMPOSITOR
  // Snapshot this backend's resolved output geometry into the compositor's
  // context (InitDrm above has selected connector/CRTC/mode). Additional
  // outputs get their own context + compositor sharing this backend's
  // EGL/GBM.
  backend->compositor_ = std::make_unique<DrmCompositor>(
      backend.get(),
      DrmOutputContext(backend->crtc_id_, backend->crtc_index_,
                       backend->connector_id_, backend->mode_, backend->fb_w_,
                       backend->fb_h_, &backend->resolved()));
  // The implicit view (id 0) scans out on this backend's own CRTC.
  backend->view_outputs_[0] = backend->compositor_.get();
  // Claim the primary CRTC so additional outputs (AddOutput) pick distinct
  // ones.
  backend->used_crtcs_.push_back(backend->crtc_id_);
#endif

  // HW cursor on the CRTC's cursor plane (or legacy drmModeSetCursor
  // when the driver doesn't expose one). Failure is non-fatal —
  // pointer events still flow to Flutter, just without a visible
  // sprite. Independent of the compositor: cursor commits run on
  // their own AtomicRequests on the seat dispatch thread.
#if HAVE_DRM_CURSOR
  if (backend->cfg_.disable_cursor) {
    ihs::log::info("[DrmBackend] HW cursor disabled (--disable-cursor)");
  } else {
    backend->cursor_ = homescreen::DrmCursor::Create(
        *backend->drm_dev_, backend->crtc_id_, backend->connector_id_,
        backend->mode_, backend->fb_w_, backend->fb_h_,
        /*rotation_degrees=*/0, backend->cfg_.cursor_theme);
  }
#if BUILD_COMPOSITOR
  // Decide whether to stage the cursor into the compositor's own atomic commit
  // or let it self-commit. Staging is only a win on nvidia-drm, where a
  // separate cursor commit on the CRTC starves the compositor's
  // PAGE_FLIP_EVENT (the cursor's blocking commit swallows the pending flip's
  // completion, freezing content on pointer motion). Everywhere else staging
  // couples cursor motion to Flutter frame production, so the cursor stalls
  // while the UI is idle — the self-committing cursor is smoother. kAuto (the
  // default) therefore stages only on nvidia-drm; yes/no force the choice.
  //
  // The raster drain (IVI_DRM_RASTER_DRAIN, on by default) removes the very
  // PAGE_FLIP_EVENT starvation that made staging necessary on nvidia-drm: the
  // rasterizer drains the flip itself, so a self-committing cursor no longer
  // swallows the pending flip. So when the drain is active, kAuto self-commits
  // on nvidia-drm too (smooth, decoupled cursor — verified on Jetson). With the
  // drain disabled (=0), kAuto reverts to staging there.
  const bool stage_cursor =
      backend->cfg_.stage_cursor == drm_config::TriState::kYes ||
      (backend->cfg_.stage_cursor == drm_config::TriState::kAuto &&
       backend->resolved_->driver_name == "nvidia-drm" &&
       !RasterDrainEnabled());
  // Either way, reserve the cursor's plane so the scene allocator's
  // disable-unused pass doesn't toggle it off every commit (flicker on
  // motion). plane_id()==0 (legacy cursor path) is a no-op in the
  // compositor.
  if (backend->cursor_ && backend->compositor_) {
    if (stage_cursor) {
      backend->cursor_->SetStagedMode(true);
      backend->compositor_->SetCursor(backend->cursor_.get());
    }
    backend->compositor_->ReserveCursorPlane(backend->cursor_->plane_id());
  }
#else
  // No compositor to stage into: a self-committing cursor on nvidia-drm
  // starves the legacy page flip on pointer motion. Drop the cursor on
  // that driver (other drivers keep the self-commit path).
  if (backend->cursor_ && backend->resolved_->driver_name == "nvidia-drm") {
    ihs::log::warn(
        "[DrmBackend] nvidia-drm without compositor: HW cursor disabled (no "
        "atomic commit to stage into)");
    backend->cursor_.reset();
  }
#endif
#endif

  // GL-composited cursor fallback: drawn into FBO 0 each Present, used when no
  // hardware cursor plane is active (HAVE_DRM_CURSOR off, no cursor plane, no
  // XCursor theme, or HW Create() returned null). Skipped under
  // --disable-cursor. Single-plane CRTCs (i.MX LCDIF, etc.) land here.
  {
    bool hw_cursor_active = false;
#if HAVE_DRM_CURSOR
    hw_cursor_active = (backend->cursor_ != nullptr);
#endif
    if (!hw_cursor_active && !backend->cfg_.disable_cursor) {
      backend->gl_cursor_ = std::make_unique<homescreen::GlCursor>();
      ihs::log::info(
          "[DrmBackend] no HW cursor plane active — using GL-composited "
          "cursor");
    }
  }

#if HAVE_DRM_CAPTURE
  backend->capture_ = homescreen::DrmCapture::Create();
#endif

  // Wire pause/resume lifecycle handlers now that the compositor exists.
  // The handlers fire on DrmSession's dispatch thread; both do only
  // atomic-flag work + (on resume) a single ScheduleFrame call — no
  // kernel ioctls — so the seat dispatch loop stays responsive.
  // DrmSeat installs its own pair separately (libinput suspend/resume).
  if (session != nullptr) {
    DrmBackend* raw = backend.get();
    session->AddPauseHandler([raw]() { raw->OnSessionPaused(); });
    session->AddResumeHandler(
        [raw](int new_fd) { raw->OnSessionResumed(new_fd); });
  }

  return backend;
}

void DrmBackend::MaybeCaptureSnapshot() {
#if HAVE_DRM_CAPTURE
  if (capture_ != nullptr && drm_dev_) {
    capture_->MaybeCapture(*drm_dev_, crtc_id_);
  }
#endif
}

// Routes drm-cxx's internal diagnostics through the ivi-homescreen
// logger so they interleave correctly with the embedder's own output
// (and respect its level / sink config) instead of going to stderr via
// drm-cxx's default print sink.
//
// Installed once on first DrmBackend construction. The drm-cxx log
// sink is process-wide; re-installing on every ctor would be harmless
// but pointless.
namespace {
void InstallDrmCxxLogSink() {
  static std::once_flag once;
  std::call_once(once, []() {
    drm::set_log_sink([](const drm::LogLevel level, const std::string_view m) {
      switch (level) {
        case drm::LogLevel::Error:
          ihs::log::error("[drm-cxx] {}", m);
          break;
        case drm::LogLevel::Warn:
          ihs::log::warn("[drm-cxx] {}", m);
          break;
        case drm::LogLevel::Info:
          ihs::log::info("[drm-cxx] {}", m);
          break;
        case drm::LogLevel::Debug:
          ihs::log::debug("[drm-cxx] {}", m);
          break;
        case drm::LogLevel::Silent:
          break;
      }
    });
  });
}
}  // namespace

DrmBackend::DrmBackend(DrmConfig cfg, homescreen::DrmSession* session)
    : cfg_(std::move(cfg)), session_(session) {
  InstallDrmCxxLogSink();
  // Unified scanout-cadence profiling, shared var with the other backends.
  // Distinct from the cfg_.debug_backend per-second FPS line in
  // RecordFlipComplete(). Enabled() also honors the umbrella IVI_PROFILE.
  vsync_.EnableProfile(profiling::FrameProfile::Enabled("IVI_VSYNC_PROFILE"),
                       "DrmVsync");
  // Motion-to-photon (IVI_M2P_PROFILE): the flip path feeds RecordPresent and
  // DrmSeat feeds RecordInput, both marshaled onto the platform task runner.
  InitMotionToPhoton();
}

homescreen::ICursorPositionSink* DrmBackend::gl_cursor() const {
  return gl_cursor_.get();
}

DrmBackend::~DrmBackend() {
  // The vsync provider's session summary (IVI_VSYNC_PROFILE) is logged from
  // vsync_.Stop() in StopVsyncMonitor, which FlutterView calls before the
  // engine destructs.

  // Let any in-flight page flip land so we don't free a BO still being
  // scanned out.
  (void)WaitForPendingFlip();

  // Release the GL-composited cursor's GL objects while the context is current.
  if (gl_cursor_ && egl_display_ != EGL_NO_DISPLAY &&
      egl_context_ != EGL_NO_CONTEXT) {
    eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_);
    gl_cursor_->Destroy();
  }
  gl_cursor_.reset();

#if BUILD_HUD
  // Free the HUD's GL objects while the context is current.
  if (hud_ && egl_display_ != EGL_NO_DISPLAY &&
      egl_context_ != EGL_NO_CONTEXT) {
    eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_);
  }
  hud_.reset();
#endif

#if BUILD_COMPOSITOR
  // Release compositor GL resources while the context is still current.
  if (egl_display_ != EGL_NO_DISPLAY && egl_context_ != EGL_NO_CONTEXT) {
    eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_);
  }
  compositor_.reset();
#endif

  if (egl_display_ != EGL_NO_DISPLAY) {
    eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    if (egl_surface_ != EGL_NO_SURFACE) {
      eglDestroySurface(egl_display_, egl_surface_);
    }
    if (egl_texture_context_ != EGL_NO_CONTEXT) {
      eglDestroyContext(egl_display_, egl_texture_context_);
    }
    if (egl_resource_context_ != EGL_NO_CONTEXT) {
      eglDestroyContext(egl_display_, egl_resource_context_);
    }
    if (egl_context_ != EGL_NO_CONTEXT) {
      eglDestroyContext(egl_display_, egl_context_);
    }
    eglTerminate(egl_display_);
  }

  if (drm_dev_ && saved_crtc_) {
    drmModeSetCrtc(drm_dev_->fd(), saved_crtc_->crtc_id, saved_crtc_->buffer_id,
                   saved_crtc_->x, saved_crtc_->y, &connector_id_, 1,
                   &saved_crtc_->mode);
    drmModeFreeCrtc(saved_crtc_);
  }

  if (drm_dev_ && pending_fb_ != 0) {
    drmModeRmFB(drm_dev_->fd(), pending_fb_);
  }
  if (pending_bo_) {
    gbm_surface_release_buffer(gbm_surface_, pending_bo_);
  }
  if (drm_dev_ && current_fb_ != 0) {
    drmModeRmFB(drm_dev_->fd(), current_fb_);
  }
  if (current_bo_) {
    gbm_surface_release_buffer(gbm_surface_, current_bo_);
  }
  if (gbm_surface_) {
    gbm_surface_destroy(gbm_surface_);
  }
  if (gbm_device_) {
    gbm_device_destroy(gbm_device_);
  }

  // The shared card + master are owned by DrmDisplay (the device-context),
  // which outlives this backend and drops master / closes the fd on its own
  // teardown. The backend holds only a non-owning pointer.
}

bool DrmBackend::InitDrm() {
  // The card is opened and held at master by the device-context
  // (DrmDisplay::SharedDevice); Create stored the non-owning pointer. The
  // backend neither opens the device nor takes master -- one card, one master,
  // shared by every view that scans out to it.
  if (drm_dev_ == nullptr) {
    ihs::log::error("[DrmBackend] no shared DRM device provided");
    return false;
  }

  if (drmSetClientCap(drm_dev_->fd(), DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) !=
      0) {
    ihs::log::warn("[DrmBackend] DRM_CLIENT_CAP_UNIVERSAL_PLANES unsupported");
  }
  if (drmSetClientCap(drm_dev_->fd(), DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
    ihs::log::warn("[DrmBackend] DRM_CLIENT_CAP_ATOMIC unsupported");
  }

  drmModeRes* res = drmModeGetResources(drm_dev_->fd());
  if (!res) {
    ihs::log::error("[DrmBackend] drmModeGetResources failed: {}",
                    std::strerror(errno));
    return false;
  }

  // Preference order for auto-pick: internal panels first, then cable-out,
  // VGA last. Copied from drm-cxx examples/common/select_connector.hpp's
  // k_main_rank — keeping it inline so the shell doesn't take a dependency
  // on the examples/ tree.
  static constexpr std::array<uint32_t, 11> kConnectorRank = {{
      DRM_MODE_CONNECTOR_eDP,
      DRM_MODE_CONNECTOR_LVDS,
      DRM_MODE_CONNECTOR_DSI,
      DRM_MODE_CONNECTOR_DPI,
      DRM_MODE_CONNECTOR_HDMIA,
      DRM_MODE_CONNECTOR_HDMIB,
      DRM_MODE_CONNECTOR_DisplayPort,
      DRM_MODE_CONNECTOR_DVID,
      DRM_MODE_CONNECTOR_DVII,
      DRM_MODE_CONNECTOR_DVIA,
      DRM_MODE_CONNECTOR_VGA,
  }};
  // Use the local ConnectorTypeName() helper (not libdrm's) so the
  // matched name is byte-identical to what --drm-list-modes prints.
  // libdrm's drmModeGetConnectorTypeName can also return NULL for
  // unknown types — std::string(NULL) is UB.
  const auto connector_name = [](const drmModeConnector* c) {
    return std::string(ConnectorTypeName(c->connector_type)) + "-" +
           std::to_string(c->connector_type_id);
  };

  // Collect every eligible connector (CONNECTED + has modes); free the
  // rest as we walk. We deliberately don't require encoder_id != 0 —
  // the CRTC-selection step below walks connector->encoders[] when the
  // current encoder is missing (hotplug or some ARM/i915 paths).
  std::vector<drmModeConnector*> eligible;
  eligible.reserve(static_cast<size_t>(res->count_connectors));
  for (int i = 0; i < res->count_connectors; ++i) {
    drmModeConnector* c =
        drmModeGetConnector(drm_dev_->fd(), res->connectors[i]);
    if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
      eligible.push_back(c);
    } else {
      drmModeFreeConnector(c);
    }
  }

  drmModeConnector* connector = nullptr;
  std::string pick_reason;
  if (cfg_.lease_connector_id != 0) {
    // A lease decides the connector for us, so this outranks both the name pin
    // and the rank heuristic below. Failing here rather than falling back:
    // every other connector on this fd is one we do not hold.
    for (drmModeConnector* c : eligible) {
      if (c->connector_id == cfg_.lease_connector_id) {
        connector = c;
        pick_reason = "lease";
        break;
      }
    }
    if (connector == nullptr) {
      ihs::log::error(
          "[DrmBackend] leased connector id {} is not eligible (absent, "
          "disconnected, or no modes); eligible connectors on this lease fd:",
          cfg_.lease_connector_id);
      for (drmModeConnector* c : eligible) {
        ihs::log::error("[DrmBackend]   {} (id {})", connector_name(c),
                        c->connector_id);
      }
      for (drmModeConnector* c : eligible) {
        drmModeFreeConnector(c);
      }
      drmModeFreeResources(res);
      return false;
    }
    if (cfg_.connector_name.has_value()) {
      // Both set means --drm-connector was given alongside a lease. The lease
      // wins; say so rather than silently ignoring the operator's pin.
      ihs::log::warn(
          "[DrmBackend] --drm-connector={} ignored: this is a leased device "
          "and the compositor leased connector {} ({}). Use --lease-connector "
          "to influence which connector is requested.",
          *cfg_.connector_name, cfg_.lease_connector_id,
          connector_name(connector));
    }
  } else if (cfg_.connector_name.has_value()) {
    const std::string& want = *cfg_.connector_name;
    for (drmModeConnector* c : eligible) {
      if (connector_name(c) == want) {
        connector = c;
        pick_reason = "user pin (--drm-connector)";
        break;
      }
    }
    if (!connector) {
      ihs::log::error(
          "[DrmBackend] --drm-connector={} not found among eligible "
          "connectors; available:",
          want);
      for (drmModeConnector* c : eligible) {
        ihs::log::error("[DrmBackend]   {}", connector_name(c));
      }
      for (drmModeConnector* c : eligible) {
        drmModeFreeConnector(c);
      }
      drmModeFreeResources(res);
      return false;
    }
  } else {
    for (const uint32_t want_type : kConnectorRank) {
      for (drmModeConnector* c : eligible) {
        if (c->connector_type == want_type) {
          connector = c;
          pick_reason = "rank";
          break;
        }
      }
      if (connector != nullptr) {
        break;
      }
    }
    if (!connector && !eligible.empty()) {
      connector = eligible.front();
      pick_reason = "first-eligible (no rank match)";
    }
  }

  if (!connector) {
    ihs::log::error("[DrmBackend] no connected connector found");
    drmModeFreeResources(res);
    return false;
  }
  for (drmModeConnector* c : eligible) {
    if (c != connector) {
      drmModeFreeConnector(c);
    }
  }
  ihs::log::info("[DrmBackend] picked connector {} via {}",
                 connector_name(connector), pick_reason);
  connector_id_ = connector->connector_id;

  // Default: drive the display at its preferred/native mode. Picking a
  // smaller mode to match a requested FB size is worse on almost every
  // axis — blurry on LCDs, worse backlight uniformity on some panels,
  // and on some connectors the mode list doesn't contain an exact match.
  // If the user specified a (smaller) FB size, we keep the preferred
  // mode for the CRTC and letterbox the requested FB inside it (see
  // fb_w_/fb_h_ below).
  //
  // Override: cfg_.mode_spec (--drm-mode=<W>x<H>@<R>) overrides the
  // preferred-mode pick. Useful for high-refresh validation when the
  // panel reports a lower refresh as preferred (e.g. 2560x1440@60Hz
  // preferred on a panel that also offers 1920x1080@120Hz).
  if (cfg_.mode_spec.has_value()) {
    // Parse "<W>x<H>@<R>" via strtoul so we catch overflow and trailing
    // garbage; sscanf("%u…") silently wraps on out-of-range input
    // (cert-err34-c). Each segment must be a non-zero unsigned int that
    // fits in uint32_t and is followed by the expected delimiter.
    const auto parse_u32 = [](const char* p, char sep, uint32_t& out,
                              const char*& next) -> bool {
      errno = 0;
      char* end = nullptr;
      const unsigned long v = std::strtoul(p, &end, 10);
      if (end == p || errno == ERANGE || v == 0 ||
          v > std::numeric_limits<uint32_t>::max() || *end != sep) {
        return false;
      }
      out = static_cast<uint32_t>(v);
      next = end + 1;  // skip the separator
      return true;
    };
    uint32_t want_w = 0;
    uint32_t want_h = 0;
    uint32_t want_r = 0;
    const char* p = cfg_.mode_spec->c_str();
    const char* p2 = nullptr;
    const char* p3 = nullptr;
    char* end = nullptr;
    bool ok = parse_u32(p, 'x', want_w, p2) && parse_u32(p2, '@', want_h, p3);
    if (ok) {
      errno = 0;
      const unsigned long r = std::strtoul(p3, &end, 10);
      ok = end != p3 && errno != ERANGE && r != 0 &&
           r <= std::numeric_limits<uint32_t>::max() && *end == '\0';
      if (ok) {
        want_r = static_cast<uint32_t>(r);
      }
    }
    if (!ok) {
      ihs::log::error(
          "[DrmBackend] --drm-mode='{}' is not in '<W>x<H>@<R>' form "
          "(e.g. 1920x1080@120)",
          *cfg_.mode_spec);
      drmModeFreeConnector(connector);
      drmModeFreeResources(res);
      return false;
    }
    for (int i = 0; i < connector->count_modes; ++i) {
      const auto& m = connector->modes[i];
      if (m.hdisplay == want_w && m.vdisplay == want_h &&
          m.vrefresh == want_r) {
        mode_ = m;
        break;
      }
    }
    if (mode_.clock == 0) {
      ihs::log::error(
          "[DrmBackend] --drm-mode='{}' not available on connector {}; run "
          "--drm-list-modes for the supported list",
          *cfg_.mode_spec, connector_name(connector));
      drmModeFreeConnector(connector);
      drmModeFreeResources(res);
      return false;
    }
    ihs::log::info("[DrmBackend] mode override: {}x{}@{}Hz (--drm-mode)",
                   mode_.hdisplay, mode_.vdisplay, mode_.vrefresh);
  } else {
    for (int i = 0; i < connector->count_modes; ++i) {
      const auto& m = connector->modes[i];
      if (m.type & DRM_MODE_TYPE_PREFERRED) {
        mode_ = m;
        break;
      }
    }
    if (mode_.clock == 0) {
      mode_ = connector->modes[0];
    }
  }

  // Framebuffer size: explicit request wins; otherwise full-screen.
  // Clamp to the mode so a too-large request still produces a valid
  // centered rect (CRTC_W > mode would fail the atomic TEST).
  fb_w_ =
      std::min<uint32_t>(cfg_.width.value_or(mode_.hdisplay), mode_.hdisplay);
  fb_h_ =
      std::min<uint32_t>(cfg_.height.value_or(mode_.vdisplay), mode_.vdisplay);

  drmModeEncoder* enc = nullptr;
  if (connector->encoder_id) {
    enc = drmModeGetEncoder(drm_dev_->fd(), connector->encoder_id);
  }
  if (enc && enc->crtc_id) {
    crtc_id_ = enc->crtc_id;
  } else {
    for (int e = 0; e < connector->count_encoders && !crtc_id_; ++e) {
      drmModeEncoder* candidate =
          drmModeGetEncoder(drm_dev_->fd(), connector->encoders[e]);
      if (!candidate) {
        continue;
      }
      for (int c = 0; c < res->count_crtcs; ++c) {
        if (candidate->possible_crtcs & (1u << c)) {
          crtc_id_ = res->crtcs[c];
          crtc_index_ = static_cast<uint32_t>(c);
          break;
        }
      }
      drmModeFreeEncoder(candidate);
    }
  }
  if (enc) {
    for (int c = 0; c < res->count_crtcs; ++c) {
      if (res->crtcs[c] == crtc_id_) {
        crtc_index_ = static_cast<uint32_t>(c);
        break;
      }
    }
    drmModeFreeEncoder(enc);
  }

  if (!crtc_id_) {
    ihs::log::error("[DrmBackend] no CRTC available for connector {}",
                    connector_id_);
    drmModeFreeConnector(connector);
    drmModeFreeResources(res);
    return false;
  }

  saved_crtc_ = drmModeGetCrtc(drm_dev_->fd(), crtc_id_);

  if (fb_w_ == mode_.hdisplay && fb_h_ == mode_.vdisplay) {
    ihs::log::info("[DrmBackend] connector={} crtc={} mode={}x{}@{}Hz",
                   connector_id_, crtc_id_, mode_.hdisplay, mode_.vdisplay,
                   mode_.vrefresh);
  } else {
    ihs::log::info(
        "[DrmBackend] connector={} crtc={} mode={}x{}@{}Hz fb={}x{} "
        "(letterboxed)",
        connector_id_, crtc_id_, mode_.hdisplay, mode_.vdisplay, mode_.vrefresh,
        fb_w_, fb_h_);
  }

  drmModeFreeConnector(connector);
  drmModeFreeResources(res);
  return true;
}

namespace {

// Walk the primary plane's IN_FORMATS blob and collect every modifier
// the kernel advertises for `fourcc`. Returns an empty vector when the
// blob is missing or `fourcc` isn't listed — the legacy API path then
// takes over. The IN_FORMATS layout uses a 64-format bitmask per
// drm_format_modifier entry; that's the `(formats >> (idx - offset))`
// shift the kernel uses internally.
std::vector<uint64_t> QueryPlaneModifiers(const int drm_fd,
                                          const uint32_t plane_id,
                                          const uint32_t fourcc) {
  std::vector<uint64_t> out;
  drmModeObjectProperties* props =
      drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
  if (props == nullptr) {
    return out;
  }
  uint32_t blob_id = 0;
  for (uint32_t i = 0; i < props->count_props && blob_id == 0; ++i) {
    if (drmModePropertyRes* p = drmModeGetProperty(drm_fd, props->props[i])) {
      if (std::string_view(p->name) == "IN_FORMATS") {
        blob_id = static_cast<uint32_t>(props->prop_values[i]);
      }
      drmModeFreeProperty(p);
    }
  }
  drmModeFreeObjectProperties(props);
  if (blob_id == 0) {
    return out;
  }
  drmModePropertyBlobRes* blob = drmModeGetPropertyBlob(drm_fd, blob_id);
  if (blob == nullptr || blob->data == nullptr) {
    if (blob != nullptr) {
      drmModeFreePropertyBlob(blob);
    }
    return out;
  }
  const auto* hdr = static_cast<const drm_format_modifier_blob*>(blob->data);
  const auto* fmts = reinterpret_cast<const uint32_t*>(
      static_cast<const char*>(blob->data) + hdr->formats_offset);
  const auto* mods = reinterpret_cast<const drm_format_modifier*>(
      static_cast<const char*>(blob->data) + hdr->modifiers_offset);
  uint32_t fmt_idx = UINT32_MAX;
  for (uint32_t i = 0; i < hdr->count_formats; ++i) {
    if (fmts[i] == fourcc) {
      fmt_idx = i;
      break;
    }
  }
  if (fmt_idx != UINT32_MAX) {
    for (uint32_t i = 0; i < hdr->count_modifiers; ++i) {
      if (fmt_idx < mods[i].offset || fmt_idx >= mods[i].offset + 64) {
        continue;
      }
      if (mods[i].formats & (1ULL << (fmt_idx - mods[i].offset))) {
        out.push_back(mods[i].modifier);
      }
    }
  }
  drmModeFreePropertyBlob(blob);
  return out;
}

}  // namespace

bool DrmBackend::InitGbm() {
  // DriverProbe returns 0 when the primary plane advertises none of the
  // formats we know how to drive; surface that clearly instead of letting
  // gbm_surface_create fail with a generic nullptr.
  if (resolved_->primary_format == 0) {
    ihs::log::error(
        "[DrmBackend] primary plane advertises no supported format "
        "(XRGB8888/XBGR8888/ARGB8888/ABGR8888/RGB565). Override with "
        "--drm-primary-format if you know what's there.");
    return false;
  }

  gbm_device_ = gbm_create_device(drm_dev_->fd());
  if (!gbm_device_) {
    ihs::log::error("[DrmBackend] gbm_create_device failed");
    return false;
  }

  // Try gbm_surface_create_with_modifiers first when the kernel
  // advertises any modifiers for our format. NVIDIA L4T's GBM only
  // implements the modifier-aware entrypoint (legacy gbm_surface_create
  // returns ENOSYS); modifier-aware also gives us the right plumbing
  // on newer Mesa drivers that require non-LINEAR tilings for scanout.
  if (resolved_->primary_plane != 0) {
    const auto modifiers = QueryPlaneModifiers(
        drm_dev_->fd(), resolved_->primary_plane, resolved_->primary_format);
    if (!modifiers.empty()) {
      gbm_surface_ = gbm_surface_create_with_modifiers(
          gbm_device_, fb_w_, fb_h_, resolved_->primary_format,
          modifiers.data(), static_cast<unsigned>(modifiers.size()));
      if (gbm_surface_) {
        ihs::log::info(
            "[DrmBackend] gbm_surface_create_with_modifiers OK "
            "(format={}, {} IN_FORMATS modifiers)",
            drm::format_name(resolved_->primary_format), modifiers.size());
        return true;
      }
      ihs::log::debug(
          "[DrmBackend] gbm_surface_create_with_modifiers failed "
          "(errno={}); falling back to legacy gbm_surface_create",
          errno);
    }
  }

  gbm_surface_ =
      gbm_surface_create(gbm_device_, fb_w_, fb_h_, resolved_->primary_format,
                         GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!gbm_surface_) {
    ihs::log::error("[DrmBackend] gbm_surface_create(format={}) failed",
                    drm::format_name(resolved_->primary_format));
    return false;
  }
  ihs::log::info("[DrmBackend] gbm_surface_create OK (format={}, legacy path)",
                 drm::format_name(resolved_->primary_format));
  return true;
}

bool DrmBackend::InitEgl() {
  const auto get_platform_display =
      reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
          eglGetProcAddress("eglGetPlatformDisplayEXT"));
  if (get_platform_display) {
    egl_display_ =
        get_platform_display(EGL_PLATFORM_GBM_KHR, gbm_device_, nullptr);
  } else {
    egl_display_ =
        eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(gbm_device_));
  }
  if (egl_display_ == EGL_NO_DISPLAY) {
    ihs::log::error("[DrmBackend] eglGetPlatformDisplay failed");
    return false;
  }

  EGLint major = 0;
  EGLint minor = 0;
  if (!eglInitialize(egl_display_, &major, &minor)) {
    ihs::log::error("[DrmBackend] eglInitialize failed: 0x{:x}", eglGetError());
    return false;
  }
  IHS_DEBUG("[DrmBackend] EGL {}.{}", major, minor);

  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    ihs::log::error("[DrmBackend] eglBindAPI failed");
    return false;
  }

  // eglChooseConfig returns any config whose bit-sizes cover the request —
  // it does not constrain EGL_NATIVE_VISUAL_ID, so the first hit often has
  // a visual that doesn't match the GBM surface and eglCreateWindowSurface
  // fails with EGL_BAD_MATCH. List all candidates and pick one whose
  // native visual matches the GBM format we used for the surface.
  const EGLint* egl_attrs = EglAttrsFor(resolved_->primary_format);
  EGLint num_configs = 0;
  if (!eglChooseConfig(egl_display_, egl_attrs, nullptr, 0, &num_configs) ||
      num_configs < 1) {
    ihs::log::error("[DrmBackend] eglChooseConfig (count) failed: 0x{:x}",
                    eglGetError());
    return false;
  }
  std::vector<EGLConfig> configs(static_cast<size_t>(num_configs));
  if (!eglChooseConfig(egl_display_, egl_attrs, configs.data(), num_configs,
                       &num_configs)) {
    ihs::log::error("[DrmBackend] eglChooseConfig (fill) failed: 0x{:x}",
                    eglGetError());
    return false;
  }

  auto attr = [this](EGLConfig c, EGLint id) -> EGLint {
    EGLint v = 0;
    eglGetConfigAttrib(egl_display_, c, id, &v);
    return v;
  };

  if (cfg_.debug_backend) {
    ihs::log::info("[DrmBackend] {} candidate EGL configs", num_configs);
    for (EGLint i = 0; i < num_configs; ++i) {
      EGLConfig c = configs[static_cast<size_t>(i)];
      ihs::log::info(
          "[DrmBackend]   [{}] visual=0x{:08x} R{}G{}B{}A{} depth={} "
          "stencil={} samples={} caveat=0x{:x} renderable=0x{:x} "
          "surface=0x{:x} conformant=0x{:x}",
          i, static_cast<unsigned>(attr(c, EGL_NATIVE_VISUAL_ID)),
          attr(c, EGL_RED_SIZE), attr(c, EGL_GREEN_SIZE),
          attr(c, EGL_BLUE_SIZE), attr(c, EGL_ALPHA_SIZE),
          attr(c, EGL_DEPTH_SIZE), attr(c, EGL_STENCIL_SIZE),
          attr(c, EGL_SAMPLES),
          static_cast<unsigned>(attr(c, EGL_CONFIG_CAVEAT)),
          static_cast<unsigned>(attr(c, EGL_RENDERABLE_TYPE)),
          static_cast<unsigned>(attr(c, EGL_SURFACE_TYPE)),
          static_cast<unsigned>(attr(c, EGL_CONFORMANT)));
    }
  }

  // Pick the best config among those whose native visual matches the GBM
  // surface format. The "best" depends on what the main window surface is
  // actually used for:
  //
  //   Compositor ON (BUILD_COMPOSITOR=1)
  //     Flutter renders every layer into an EglFboBackingStore that owns
  //     its own depth+stencil; the main surface only composites textured
  //     quads into FBO 0. It does not depth-test or stencil, so we prefer
  //     the thinnest window surface: stencil=0, the smallest depth.
  //
  //   Compositor OFF
  //     Flutter renders directly into the default framebuffer and needs
  //     stencil for clip-path operations. Prefer stencil=8 on the main
  //     surface.
  //
  // MSAA is never desirable for scanout (the kernel only reads single-
  // sample pixels, so any MSAA work is resolved and thrown away), and
  // caveat=EGL_NONE is always preferred over slow/non-conformant flags.
#if BUILD_COMPOSITOR
  constexpr EGLint kPreferredStencil = 0;
#else
  constexpr EGLint kPreferredStencil = 8;
#endif
  const EGLint preferred_alpha = PreferredAlphaFor(resolved_->primary_format);

  auto abs_diff = [](const EGLint a, const EGLint b) -> EGLint {
    return a > b ? a - b : b - a;
  };

  // Lower tuple = better. std::tuple's lexicographic operator< gives us
  // strict priority ordering without nested branches.
  using Score = std::tuple<EGLint, EGLint, EGLint, EGLint, EGLint>;
  auto score = [&](const EGLint i) -> Score {
    EGLConfig c = configs[static_cast<size_t>(i)];
    return {
        attr(c, EGL_SAMPLES),                                    // 1
        attr(c, EGL_CONFIG_CAVEAT) == EGL_NONE ? 0 : 1,          // 2
        abs_diff(attr(c, EGL_ALPHA_SIZE), preferred_alpha),      // 3
        abs_diff(attr(c, EGL_STENCIL_SIZE), kPreferredStencil),  // 4
        attr(c, EGL_DEPTH_SIZE),                                 // 5
    };
  };

  egl_config_ = nullptr;
  EGLint best_idx = -1;
  for (EGLint i = 0; i < num_configs; ++i) {
    if (attr(configs[static_cast<size_t>(i)], EGL_NATIVE_VISUAL_ID) !=
        static_cast<EGLint>(resolved_->primary_format)) {
      continue;
    }
    if (best_idx < 0 || score(i) < score(best_idx)) {
      best_idx = i;
    }
  }
  if (best_idx < 0) {
    ihs::log::error(
        "[DrmBackend] no EGL config matches GBM format 0x{:x} (checked {} "
        "configs)",
        resolved_->primary_format, num_configs);
    return false;
  }
  egl_config_ = configs[static_cast<size_t>(best_idx)];
  if (cfg_.debug_backend) {
    ihs::log::info("[DrmBackend] selected config [{}] (compositor={})",
                   best_idx, BUILD_COMPOSITOR ? "on" : "off");
  }

  egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT,
                                  kEsContextAttribs.data());
  if (egl_context_ == EGL_NO_CONTEXT) {
    ihs::log::error("[DrmBackend] eglCreateContext failed: 0x{:x}",
                    eglGetError());
    return false;
  }
  egl_resource_context_ = eglCreateContext(
      egl_display_, egl_config_, egl_context_, kEsContextAttribs.data());
  egl_texture_context_ = eglCreateContext(
      egl_display_, egl_config_, egl_context_, kEsContextAttribs.data());

  egl_surface_ = eglCreateWindowSurface(
      egl_display_, egl_config_,
      reinterpret_cast<EGLNativeWindowType>(gbm_surface_), nullptr);
  if (egl_surface_ == EGL_NO_SURFACE) {
    ihs::log::error("[DrmBackend] eglCreateWindowSurface failed: 0x{:x}",
                    eglGetError());
    return false;
  }
  return true;
}

uint32_t DrmBackend::AddFb(gbm_bo* bo) const {
  const uint32_t width = gbm_bo_get_width(bo);
  const uint32_t height = gbm_bo_get_height(bo);
  const uint32_t format = gbm_bo_get_format(bo);
  const uint64_t modifier = gbm_bo_get_modifier(bo);
  const int plane_count = gbm_bo_get_plane_count(bo);

  // The gbm_surface is created with_modifiers, so on tiled/compressed
  // allocators (e.g. the Mali blob's AFBC on rk3588) the front buffer
  // carries a non-linear modifier. The legacy drmModeAddFB is
  // modifier-blind and tells KMS the buffer is linear — the display
  // controller then scans tiled data as linear and underflows
  // (rockchip VOP2 POST_BUF_EMPTY → green/black field). Propagate the
  // real modifier via drmModeAddFB2WithModifiers, mirroring
  // DrmCompositor::ImportBoAsFb.
  uint32_t handles[4] = {};
  uint32_t pitches[4] = {};
  uint32_t offsets[4] = {};
  uint64_t modifiers[4] = {};
  for (int i = 0; i < plane_count && i < 4; ++i) {
    handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
    pitches[i] = gbm_bo_get_stride_for_plane(bo, i);
    offsets[i] = gbm_bo_get_offset(bo, i);
    modifiers[i] = modifier;
  }

  uint32_t fb_id = 0;
  const bool use_modifiers = (modifier != DRM_FORMAT_MOD_INVALID) &&
                             (modifier != DRM_FORMAT_MOD_LINEAR);
  if (use_modifiers) {
    if (drmModeAddFB2WithModifiers(drm_dev_->fd(), width, height, format,
                                   handles, pitches, offsets, modifiers, &fb_id,
                                   DRM_MODE_FB_MODIFIERS) != 0) {
      ihs::log::error(
          "[DrmBackend] drmModeAddFB2WithModifiers({}x{}, fmt=0x{:08x}, "
          "mod=0x{:016x}, planes={}): {}",
          width, height, format, modifier, plane_count, std::strerror(errno));
      return 0;
    }
  } else if (drmModeAddFB2(drm_dev_->fd(), width, height, format, handles,
                           pitches, offsets, &fb_id, 0) != 0) {
    ihs::log::error(
        "[DrmBackend] drmModeAddFB2({}x{}, fmt=0x{:08x}, linear): {}", width,
        height, format, std::strerror(errno));
    return 0;
  }
  if (cfg_.debug_backend) {
    ihs::log::debug(
        "[DrmBackend] AddFb fb_id={} {}x{} fmt=0x{:08x} mod=0x{:016x} "
        "planes={} ({})",
        fb_id, width, height, format, modifier, plane_count,
        use_modifiers ? "modifier-aware" : "linear");
  }
  return fb_id;
}

bool DrmBackend::SetInitialMode() {
  if (!current_bo_ || current_fb_ == 0) {
    return false;
  }
  // Legacy drmModeSetCrtc scans the FB at (0,0) on the CRTC and requires
  // FB dims ≥ mode dims. Framed mode (FB smaller than mode) can't be
  // centered on this path without a scratch mode-sized FB + per-frame
  // memcpy — rejected in favor of failing loudly so the user sees the
  // plane-compositor regression that pushed us here.
  if (fb_w_ != mode_.hdisplay || fb_h_ != mode_.vdisplay) {
    ihs::log::error(
        "[DrmBackend] legacy modeset reached with framed FB ({}x{} on "
        "{}x{} mode); atomic plane-compositor must succeed for framing. "
        "Remove view.width/view.height from config to run full-screen.",
        fb_w_, fb_h_, mode_.hdisplay, mode_.vdisplay);
    return false;
  }
  if (drmModeSetCrtc(drm_dev_->fd(), crtc_id_, current_fb_, 0, 0,
                     &connector_id_, 1, &mode_) != 0) {
    ihs::log::error("[DrmBackend] drmModeSetCrtc: {}", std::strerror(errno));
    return false;
  }
  mode_set_ = true;
  return true;
}

bool DrmBackend::MakeCurrent() const {
  return eglMakeCurrent(egl_display_, egl_surface_, egl_surface_,
                        egl_context_) == EGL_TRUE;
}

bool DrmBackend::ClearCurrent() const {
  return eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        EGL_NO_CONTEXT) == EGL_TRUE;
}

bool DrmBackend::MakeResourceCurrent() const {
  return eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        egl_resource_context_) == EGL_TRUE;
}

bool DrmBackend::TextureMakeCurrent() {
  return eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        egl_texture_context_) == EGL_TRUE;
}

bool DrmBackend::TextureClearCurrent() {
  return eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        EGL_NO_CONTEXT) == EGL_TRUE;
}

void DrmBackend::RecordFlipComplete() {
  // Scanout-cadence profiling now lives in vsync_ (recorded per page flip via
  // DeliverVsync under IVI_VSYNC_PROFILE). This is just the debug_backend
  // per-second FPS line.
  if (!cfg_.debug_backend) {
    return;
  }
  const uint64_t now = LibFlutterEngine->GetCurrentTime();

  if (flip_submit_ns_ != 0) {
    [[maybe_unused]] const double latency_ms =
        static_cast<double>(now - flip_submit_ns_) / 1e6;
    IHS_DEBUG("[DrmBackend] flip latency: {:.2f} ms", latency_ms);
    flip_submit_ns_ = 0;
  }

  ++frame_count_;
  if (fps_epoch_ns_ == 0) {
    fps_epoch_ns_ = now;
  }
  if (const double elapsed_s = static_cast<double>(now - fps_epoch_ns_) / 1e9;
      elapsed_s >= 1.0) {
    ihs::log::info("[DrmBackend] FPS: {:.1f} ({} frames / {:.2f}s)",
                   frame_count_ / elapsed_s, frame_count_, elapsed_s);
    frame_count_ = 0;
    fps_epoch_ns_ = now;
  }
}

VsyncCallback DrmBackend::GetVsyncCallback() const {
  // IVI_DRM_VSYNC=0 disables vsync_callback wiring and lets Flutter's
  // wall-clock scheduler drive pacing. Useful for bisecting pacing
  // regressions or running on hardware where PAGE_FLIP_EVENT delivery
  // is unreliable. Anything else (unset, "1", arbitrary string) leaves
  // vsync_callback enabled.
  static const bool enabled = []() {
    const char* env = std::getenv("IVI_DRM_VSYNC");
    return !(env != nullptr && std::string_view(env) == "0");
  }();
  return enabled ? &VsyncTrampoline : nullptr;
}

void DrmBackend::VsyncTrampoline(void* user_data, const intptr_t baton) {
  // user_data is the FlutterDesktopEngineState* passed as userdata to
  // FlutterEngineInitialize. Recover the engine + backend from it; if
  // either is gone (shutdown race) drop the baton — leaking is the
  // documented price of an unresponded baton but the only safe option.
  auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  if (state == nullptr || state->view_controller == nullptr ||
      state->view_controller->engine == nullptr) {
    return;
  }
  auto* engine_obj = state->view_controller->engine;
  auto* backend = dynamic_cast<DrmBackend*>(engine_obj->GetBackend());
  if (backend == nullptr) {
    return;
  }
  backend->SetVsyncBaton(engine_obj->GetFlutterEngine(), baton);
}

bool DrmBackend::DrmVsyncProvider::IsSourcePending() const {
  // Mirror the pre-fold flip-in-flight test: the legacy page-flip latch,
  // unless an active compositor owns scanout — then its commit latch is
  // authoritative (a backing-store fallback frame still flips through the
  // legacy path, but planes_active() gates which latch to trust).
  bool flip = backend_->flip_pending_.load(std::memory_order_acquire);
#if BUILD_COMPOSITOR
  if (backend_->compositor_ && backend_->compositor_->planes_active()) {
    flip = backend_->compositor_->IsFlipPending();
  }
#endif
  return flip;
}

uint32_t DrmBackend::DrmVsyncProvider::PeriodNs() const {
  const uint32_t vr = backend_->mode_.vrefresh;
  return vr > 0 ? static_cast<uint32_t>(1000000000ULL / vr) : 16666667U;
}

void DrmBackend::SetVsyncBaton(FLUTTER_API_SYMBOL(FlutterEngine) engine,
                               const intptr_t baton) {
  // Cache the engine handle for OnSessionResumed (which needs it to
  // ScheduleFrame), then hand the baton to the provider. SubmitBaton stores
  // it, and — via our IsSourcePending() override — either waits for the
  // in-flight page flip to return it or drains it inline for an idle pipeline
  // (first frame / post-resume / waking on input), leaving it parked if the
  // runner isn't wired yet so the #210 kick latch delivers it (SetEngine).
  engine_handle_.store(engine, std::memory_order_release);
  vsync_.SubmitBaton(engine, baton);
}

void DrmBackend::OnSessionPaused() {
  ihs::log::info("[DrmBackend] session paused (VT switch-out)");
  session_paused_.store(true, std::memory_order_release);
#if BUILD_COMPOSITOR
  if (compositor_) {
    compositor_->SetPaused(true);
  }
#endif
  // Drop the legacy-path flip latch too. PAGE_FLIP_EVENT won't come for
  // a commit submitted before the revoke; the next Present after resume
  // will start a fresh flip cycle.
  flip_pending_.store(false, std::memory_order_release);
}

void DrmBackend::OnSessionResumed(const int new_fd) {
  // preserve-fd contract: the new fd should equal the one we took at
  // init. If it doesn't, something has changed about the underlying
  // libseat backend's behavior — log loudly because the rest of the
  // backend assumes per-fd state survived the round trip.
  if (drm_dev_ && new_fd != drm_dev_->fd()) {
    ihs::log::error(
        "[DrmBackend] resume: fd changed ({} -> {}) — preserve-fd contract "
        "violated; per-fd state (FB ids, EGL/GBM, property cache) is stale "
        "and the next commit will likely fail",
        drm_dev_->fd(), new_fd);
  }
  ihs::log::info("[DrmBackend] session resumed (VT switch-in, fd={})", new_fd);
  session_paused_.store(false, std::memory_order_release);
#if BUILD_COMPOSITOR
  if (compositor_) {
    compositor_->OnResume();
  }
#endif
  mode_set_ = false;

  // Post-resume re-modeset is blocking with no PAGE_FLIP_EVENT (same as
  // initial startup). SetVsyncBaton's flip-in-flight check handles
  // this automatically: it sees flip_pending_=false and kicks any
  // baton inline. We just need to make sure Flutter actually asks for
  // a baton (idle UI case) below.

  auto* engine = engine_handle_.load(std::memory_order_acquire);
  if (engine == nullptr) {
    ihs::log::warn(
        "[DrmBackend] resume: no engine handle yet; redraw skipped (UI must "
        "tick on its own)");
    return;
  }

  // Two cases:
  //  - Flutter was parked on a stored baton when pause hit (vsync_callback
  //    enabled, idle UI waiting on OnVsync): drain it now so Flutter
  //    wakes and renders the re-modeset frame. PostOnVsync routes the
  //    delivery through the platform task runner.
  //  - No pending baton (wall-clock pacer, or no in-flight vsync request):
  //    ScheduleFrame to prompt Flutter into a new frame request, which
  //    our reset kick will deliver as the first-baton inline-fire.
  if (vsync_.DeliverParkedBaton()) {
    ihs::log::info("[DrmBackend] resume: drained pending baton");
  } else {
    const FlutterEngineResult r = LibFlutterEngine->ScheduleFrame(engine);
    if (r != kSuccess) {
      ihs::log::warn("[DrmBackend] resume: ScheduleFrame failed (rc={})",
                     static_cast<int>(r));
    } else {
      ihs::log::info("[DrmBackend] resume: ScheduleFrame requested (idle UI)");
    }
  }
}

void DrmBackend::OnLegacyFlipComplete() {
  // The page flip just promoted pending → scanout. What was scanned out
  // before is now safe to release.
  if (current_fb_ != 0) {
    drmModeRmFB(drm_dev_->fd(), current_fb_);
  }
  if (current_bo_) {
    gbm_surface_release_buffer(gbm_surface_, current_bo_);
  }
  current_bo_ = pending_bo_;
  current_fb_ = pending_fb_;
  pending_bo_ = nullptr;
  pending_fb_ = 0;
  flip_pending_.store(false, std::memory_order_release);
  RecordFlipComplete();
}

void DrmBackend::UnifiedPageFlipHandler(int /*fd*/,
                                        unsigned int /*sequence*/,
                                        unsigned int tv_sec,
                                        unsigned int tv_usec,
                                        void* user_data) {
  // Diagnostic: log every Nth flip to see the asio monitor cadence.
  // Set IVI_DRM_FLIP_TRACE=1 to log every flip (very chatty); unset or
  // anything else logs every 60th (≈ once per second at 60Hz).
  static const bool flip_trace = []() {
    const char* env = std::getenv("IVI_DRM_FLIP_TRACE");
    return env != nullptr && std::string_view(env) == "1";
  }();
  static std::atomic<uint64_t> flip_count{0};
  const uint64_t n = flip_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (flip_trace || (n % 60) == 0) {
    ihs::log::info("[DrmBackend] flip #{} handled (asio monitor)", n);
  }
  // Route the completion to the exact committer: the backend for a legacy
  // Present() page flip, or a compositor for a scene/atomic commit — each
  // registered itself as the flip's IFlipSink. The sink records the flip and,
  // for the vsync-driving output, returns the baton (see DeliverVsyncFromFlip).
  // This lets one backend's flip reader serve several compositors (one per
  // output) without a CRTC id in the v2 event.
  static_cast<IFlipSink*>(user_data)->OnFlipEvent(tv_sec, tv_usec);
}

void DrmBackend::DeliverVsyncFromFlip(const unsigned int tv_sec,
                                      const unsigned int tv_usec) {
  // Return the vsync baton stamped with the kernel-provided scanout time
  // (tv_sec/tv_usec from the PAGE_FLIP_EVENT) rather than the handler-dispatch
  // wall clock, so the [DrmVsync] cadence isn't inflated by asio scheduling
  // jitter (matches the software sink). Already on the platform task runner
  // thread, but DeliverVsync still marshals OnVsync onto the runner strand for
  // uniformity; the one extra task hop is harmless.
  const uint64_t tv_ns = static_cast<uint64_t>(tv_sec) * 1'000'000'000ULL +
                         static_cast<uint64_t>(tv_usec) * 1000ULL;
  // Record the scanout endpoint for motion-to-photon before returning the
  // baton. The cutoff is the frame_start of the most recent baton — the input
  // cutoff of the frame this flip presents. Marshaled onto the platform runner
  // so it shares a thread with DrmSeat's RecordInput.
  profiling::MarshalRecordPresent(
      platform_task_runner_.load(std::memory_order_acquire),
      GetMotionToPhoton(), tv_ns, vsync_.LastDeliveredFrameStartNs(), "drm");
  vsync_.DeliverVsync(tv_ns);
}

void DrmBackend::OnFlipEvent(const unsigned int tv_sec,
                             const unsigned int tv_usec) {
  OnLegacyFlipComplete();
  // A legacy Present() flip belongs to this backend's own (primary) output, so
  // it always drives vsync.
  DeliverVsyncFromFlip(tv_sec, tv_usec);
}

void DrmBackend::SetPlatformTaskRunner(TaskRunner* runner) {
  platform_task_runner_.store(runner, std::memory_order_release);

  // Wire engine+runner into the provider. SetEngine's #210 kick latch drains
  // any baton parked before the runner came up (the engine's first vsync
  // request can race this call at cold start; SubmitBaton parks it when the
  // runner is unwired). Without that drain the first baton is never answered
  // and the app hangs on a black screen (only the HW cursor) until restart.
  vsync_.SetEngine(engine_handle_.load(std::memory_order_acquire), runner);
}

void DrmBackend::StopVsyncMonitor() {
  platform_task_runner_.store(nullptr, std::memory_order_release);

  // Drop any parked baton, clear engine/runner, and log the IVI_VSYNC_PROFILE
  // session summary. Any page-flip event the reader drains after this finds the
  // provider cleared and no-ops its DeliverVsync — correct, we're tearing down.
  vsync_.Stop();

  // Motion-to-photon session summary. Runs on the platform thread, the same
  // thread the marshaled RecordInput/RecordPresent tasks execute on.
  if (m2p_enabled_) {
    m2p_.LogSessionSummary("drm");
  }

  // Wait for any in-flight PAGE_FLIP_EVENT to be drained by the card's flip
  // reader (owned by DrmDisplay, still running through our teardown) so
  // ~DrmCompositor → WaitForPendingFlip does not spin. The backend must NOT
  // read the fd itself -- the reader is the single reader for the shared card.
  // Poll the flag with a one-vblank margin, then force-clear if the kernel
  // never fired the event (commit dropped, session paused, ...) so destructors
  // make progress.
  if (flip_pending_.load(std::memory_order_acquire)) {
    constexpr int kDrainTimeoutMs = 100;
    constexpr int kPollSliceMs = 2;
    int elapsed_ms = 0;
    while (elapsed_ms < kDrainTimeoutMs &&
           flip_pending_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kPollSliceMs));
      elapsed_ms += kPollSliceMs;
    }
    if (flip_pending_.load(std::memory_order_acquire)) {
      ihs::log::warn(
          "[DrmBackend] StopVsyncMonitor: flip event never arrived, "
          "force-clearing flip_pending_ to unblock destructors");
      flip_pending_.store(false, std::memory_order_release);
#if BUILD_COMPOSITOR
      if (compositor_) {
        compositor_->OnFlipComplete();
      }
#endif
    }
  }
}

bool DrmBackend::RasterDrainEnabled() {
  static const bool enabled = []() {
    const char* env = std::getenv("IVI_DRM_RASTER_DRAIN");
    // On by default — validated across 6 platforms / 5 GPU vendors (no
    // regression, no deadlock, pause/resume + teardown clean). The only opt-out
    // is an explicit "0"; anything else (incl. unset) enables it.
    return env == nullptr || std::string_view(env) != "0";
  }();
  return enabled;
}

bool DrmBackend::WaitForPendingFlip() const {
  using namespace std::chrono_literals;
  // Actively drain this flip's PAGE_FLIP_EVENT rather than passively waiting
  // for the card's reader thread to do it. That thread is unprioritized, so
  // under heavy raster load (software GL on a GPU-less board) it can be
  // CPU-starved and miss the deadline below -- leaving flip_pending_ set so the
  // next drmModePageFlip returns EBUSY, and every subsequent flip cascades. We
  // submitted the flip, we are blocked on it, so we drain it ourselves; the
  // drain is mutex-serialized with the reader thread (see DrainReadyFlips), so
  // the fd is never read twice. The reader thread still handles idle-time
  // flips.
  //
  // Bounded wait as a hang-guard: on nvidia-drm a flip's PAGE_FLIP_EVENT can go
  // genuinely undelivered, and an unbounded loop would hang Present and
  // ~DrmBackend so the process never exits on SIGTERM. Cap and proceed.
  constexpr auto kFlipWaitTimeout = 100ms;
  const auto deadline = std::chrono::steady_clock::now() + kFlipWaitTimeout;
  while (flip_pending_.load(std::memory_order_acquire)) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      ihs::log::warn(
          "[DrmBackend] WaitForPendingFlip: no flip completion after 100ms; "
          "proceeding (PAGE_FLIP_EVENT likely lost)");
      return true;
    }
    if (drm_display_ != nullptr) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
              .count();
      // Blocks up to the remaining budget waiting for the event, then drains
      // it (clearing flip_pending_ via the handler). Falls through to re-check
      // the flag when it returns.
      drm_display_->DrainReadyFlips(static_cast<int>(remaining));
    } else {
      std::this_thread::sleep_for(200us);
    }
  }
  return true;
}

#if BUILD_HUD
bool DrmBackend::EnsureHud() {
  if (!hud_checked_) {
    hud_checked_ = true;
    hud_enabled_ = std::getenv("IVI_HUD") != nullptr || hud_config_enable_;
  }
  if (!hud_enabled_ || hud_init_failed_) {
    return false;
  }
  if (!hud_) {
    std::string err;
    hud_ = ihs::hud::GlHud::Create(err);  // GL context is current in present
    if (!hud_) {
      hud_init_failed_ = true;
      ihs::log::warn("[DrmBackend] HUD unavailable ({})", err);
      return false;
    }
    hud_->SetConfig(hud_config_);
    hud_->SetOpen(true);
    ihs::log::info("[DrmBackend] debug HUD enabled");
  }
  return true;
}

ihs::hud::HudStats DrmBackend::ComputeHudStats(float& dt_s) {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const uint64_t now_ns = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
                          static_cast<uint64_t>(ts.tv_nsec);
  dt_s = 1.0f / 60.0f;
  if (hud_last_present_ns_ != 0 && now_ns > hud_last_present_ns_) {
    const uint64_t dt = now_ns - hud_last_present_ns_;
    dt_s = static_cast<float>(dt) / 1e9f;
    hud_interval_ms_[hud_interval_head_] = static_cast<float>(dt) / 1e6f;
    hud_interval_head_ = (hud_interval_head_ + 1) % hud_interval_ms_.size();
    if (hud_interval_count_ < hud_interval_ms_.size()) {
      ++hud_interval_count_;
    }
  }
  hud_last_present_ns_ = now_ns;

  ihs::hud::HudStats stats{};
  if (hud_interval_count_ > 0) {
    float sum = 0.0f;
    float worst = 0.0f;
    for (uint32_t i = 0; i < hud_interval_count_; ++i) {
      sum += hud_interval_ms_[i];
      worst = std::max(worst, hud_interval_ms_[i]);
    }
    stats.frame_ms = sum / static_cast<float>(hud_interval_count_);
    stats.frame_max_ms = worst;
    stats.fps = stats.frame_ms > 0.0f ? 1000.0f / stats.frame_ms : 0.0f;
  }
  stats.target_fps =
      mode_.vrefresh > 0 ? static_cast<float>(mode_.vrefresh) : 60.0f;
  return stats;
}

void DrmBackend::MaybeRenderHud(const FlutterLayer** layers, size_t count) {
  if (!EnsureHud()) {
    return;
  }
  float dt_s = 0.0f;
  const ihs::hud::HudStats stats = ComputeHudStats(dt_s);

  std::vector<ihs::hud::HudViewSample> views;
  for (size_t i = 0; layers != nullptr && i < count; ++i) {
    const FlutterLayer* layer = layers[i];
    if (layer != nullptr &&
        layer->type == kFlutterLayerContentTypePlatformView &&
        layer->platform_view != nullptr) {
      views.push_back({layer->platform_view->identifier,
                       static_cast<uint32_t>(layer->size.width),
                       static_cast<uint32_t>(layer->size.height),
                       /*dmabuf=*/false});
    }
  }

  hud_->Render(fb_w_, fb_h_, dt_s, stats, views);
}

unsigned int DrmBackend::RenderHudTexture(uint32_t width, uint32_t height) {
  if (!EnsureHud()) {
    return 0;
  }
  float dt_s = 0.0f;
  const ihs::hud::HudStats stats = ComputeHudStats(dt_s);
  // The plane compositor doesn't pass the layer list here; per-view rows are
  // omitted (frame stats + histogram still show).
  return hud_->RenderOffscreen(width, height, dt_s, stats, {});
}
#endif  // BUILD_HUD

bool DrmBackend::Present() {
  // Session-pause gate (mirrors DrmCompositor::PresentLayers). While the VT is
  // switched away, scanout is revoked and page flips never complete, so the
  // gbm_surface's buffers are never released. eglSwapBuffers would then block
  // forever in gbm_surface_get_free_buffer, wedging the rasterizer thread —
  // FlutterEngineShutdown can't join it and teardown hangs (SIGKILL). Ack the
  // frame without touching GL/KMS; OnSessionResumed re-modesets and restarts
  // the pacer.
  if (session_paused_.load(std::memory_order_acquire)) {
    return true;
  }
  // Lease-revocation gate. Same shape as the session-pause gate above and for
  // the same reason: the leased KMS objects are gone from this fd's view, so
  // page flips naming them fail and never complete, and eglSwapBuffers would
  // eventually block forever in gbm_surface_get_free_buffer with no buffer ever
  // released -- wedging the rasterizer thread so FlutterEngineShutdown cannot
  // join it. Unlike a VT pause this never resolves on its own: OnSessionResumed
  // has no counterpart until reacquire, so the exit policy is the way out.
  // Ack the frame without touching GL/KMS.
  if (cfg_.lease_revoked && cfg_.lease_revoked()) {
    if (!lease_revoked_logged_.exchange(true, std::memory_order_relaxed)) {
      ihs::log::warn(
          "[DrmBackend] lease revoked — gating presents; the panel stops "
          "updating until the lease is reacquired");
    }
    return true;
  }
  // Reclaim a buffer orphaned by a flip dropped during a VT-switch pause:
  // OnSessionPaused clears flip_pending_, but the in-flight flip's buffer stays
  // locked (its completion event never fires). Release it before swapping, or
  // each pause cycle leaks a locked buffer until the surface starves and
  // eglSwapBuffers blocks forever in gbm_surface_get_free_buffer (rasterizer
  // wedged → teardown SIGKILL). Race-free on the raster thread: flip_pending_
  // is false here, so the platform flip-handler won't touch pending_bo_; the
  // acquire-load pairs with OnLegacyFlipComplete's release-store so we never
  // observe a half-updated pending_bo_.
  if (!flip_pending_.load(std::memory_order_acquire) && pending_bo_) {
    if (pending_fb_ != 0 && drm_dev_) {
      drmModeRmFB(drm_dev_->fd(), pending_fb_);
      pending_fb_ = 0;
    }
    gbm_surface_release_buffer(gbm_surface_, pending_bo_);
    pending_bo_ = nullptr;
  }
  MaybeCaptureSnapshot();
  if (!WaitForPendingFlip()) {
    return false;
  }

  // Composite the cursor over the rendered Flutter scene (still in FBO 0's
  // back buffer) before the swap. No-op until the cursor is visible / absent.
  if (gl_cursor_) {
    gl_cursor_->Draw(fb_w_, fb_h_);
  }

#if BUILD_HUD
  // Draw the debug HUD over FBO 0 before the swap. This is the GL-swap path;
  // the plane-compositor path is not covered. No layer list here, so per-view
  // rows are omitted (stats + histogram still show).
  MaybeRenderHud(nullptr, 0);
#endif

  if (!eglSwapBuffers(egl_display_, egl_surface_)) {
    ihs::log::error("[DrmBackend] eglSwapBuffers: 0x{:x}", eglGetError());
    return false;
  }

  gbm_bo* next_bo = gbm_surface_lock_front_buffer(gbm_surface_);
  if (!next_bo) {
    ihs::log::error("[DrmBackend] gbm_surface_lock_front_buffer failed");
    return false;
  }

  const uint32_t next_fb = AddFb(next_bo);
  if (next_fb == 0) {
    gbm_surface_release_buffer(gbm_surface_, next_bo);
    return false;
  }

  if (!mode_set_) {
    // A resume re-modesets from this fresh buffer (mode_set_ was cleared in
    // OnSessionResumed). Release the pre-pause scanout buffer first, or it
    // leaks when overwritten below — another per-cycle starvation source.
    // Null at the first-ever modeset, so a no-op there.
    if (current_fb_ != 0 && drm_dev_) {
      drmModeRmFB(drm_dev_->fd(), current_fb_);
    }
    if (current_bo_) {
      gbm_surface_release_buffer(gbm_surface_, current_bo_);
    }
    current_bo_ = next_bo;
    current_fb_ = next_fb;
    if (!SetInitialMode()) {
      return false;
    }
    RecordFlipComplete();
    // Mirror the compositor's first-commit drain: drmModeSetCrtc produces no
    // PAGE_FLIP_EVENT, so the next baton (Flutter requested it after the first
    // OnVsync) would sit parked unfired. Drain it now via the provider;
    // subsequent batons go through the normal PageFlipHandler path.
    (void)vsync_.DeliverParkedBaton();
    return true;
  }

  if (cfg_.debug_backend) {
    flip_submit_ns_ = LibFlutterEngine->GetCurrentTime();
  }
  if (drmModePageFlip(drm_dev_->fd(), crtc_id_, next_fb,
                      DRM_MODE_PAGE_FLIP_EVENT,
                      static_cast<IFlipSink*>(this)) != 0) {
    ihs::log::warn("[DrmBackend] drmModePageFlip: {}", std::strerror(errno));
    drmModeRmFB(drm_dev_->fd(), next_fb);
    gbm_surface_release_buffer(gbm_surface_, next_bo);
    return false;
  }

  // The kernel now owns next_bo/next_fb until the page-flip-complete event
  // fires. current_bo_/current_fb_ remain the live scanout until then.
  pending_bo_ = next_bo;
  pending_fb_ = next_fb;
  flip_pending_.store(true, std::memory_order_release);
  return true;
}

void DrmBackend::Resize(size_t /*index*/,
                        Engine* /*engine*/,
                        int32_t /*w*/,
                        int32_t /*h*/) {
  // DRM/KMS mode is fixed at the connector; the engine is driven by the initial
  // mode resolution. Runtime mode switches are not supported in this phase.
}

FlutterRendererConfig DrmBackend::GetRenderConfig() {
  FlutterRendererConfig config{};
  config.type = kOpenGL;
  config.open_gl.struct_size = sizeof(FlutterOpenGLRendererConfig);

  config.open_gl.make_current = [](void* user_data) -> bool {
    return BackendFromState(user_data)->MakeCurrent();
  };
  config.open_gl.clear_current = [](void* user_data) -> bool {
    return BackendFromState(user_data)->ClearCurrent();
  };
  config.open_gl.present = [](void* user_data) -> bool {
    auto* b = BackendFromState(user_data);
#if BUILD_COMPOSITOR
    // When the plane compositor owns scanout, DrmCompositor::PresentLayers
    // has already committed the frame atomically — the engine's renderer
    // .present call is a stale no-op for this path. Doing eglSwapBuffers
    // + drmModePageFlip here would collide with the atomic commit on the
    // same CRTC. Only run the legacy Present when planes aren't active
    // (fallback latched or probe disabled planes in the first place).
    if (b->compositor_ && b->compositor_->planes_active()) {
      return true;
    }
#endif
    return b->Present();
  };
  config.open_gl.fbo_callback = [](void* /*user_data*/) -> uint32_t {
    return 0;  // window FBO
  };
  config.open_gl.make_resource_current = [](void* user_data) -> bool {
    return BackendFromState(user_data)->MakeResourceCurrent();
  };
  config.open_gl.fbo_reset_after_present = false;
  config.open_gl.gl_proc_resolver = [](void* /*userdata*/,
                                       const char* name) -> void* {
    return GlProcessResolver::GetInstance().process_resolver(name);
  };
  return config;
}

FlutterCompositor DrmBackend::GetCompositorConfig() {
  FlutterCompositor compositor{};
  compositor.struct_size = sizeof(FlutterCompositor);
  compositor.user_data = this;

#if BUILD_COMPOSITOR
  // avoid_backing_store_cache = true forces the engine to allocate/pool
  // fresh backing stores each frame, giving us the multi-BO supply the
  // plane-direct scanout path needs. With it false, Flutter reuses a
  // single BO frame-to-frame — fine for a GL fallback that blits into
  // FBO 0 each time, but catastrophic for direct scanout: the kernel
  // may optimize away a "flip to the same FB" and never fire a
  // PAGE_FLIP_EVENT, and even if it does, Flutter renders into the
  // live-scanout BO on the next frame.
  compositor.avoid_backing_store_cache = true;
  compositor.create_backing_store_callback =
      [](const FlutterBackingStoreConfig* config, FlutterBackingStore* out,
         void* user_data) -> bool {
    auto* self = static_cast<DrmBackend*>(user_data);
    return self->CompositorForView(config->view_id)
        ->CreateBackingStore(config, out);
  };
  compositor.collect_backing_store_callback =
      [](const FlutterBackingStore* store, void* user_data) -> bool {
    // Route to the compositor that created the store (each output owns its
    // pool); fall back to the primary if the store carries no owner.
    auto* self = static_cast<DrmBackend*>(user_data);
    DrmCompositor* owner = DrmCompositor::OwnerOf(store);
    return (owner != nullptr ? owner : self->compositor_.get())
        ->CollectBackingStore(store);
  };
  // Multi-view present: route per view_id. present_view_callback is
  // mutually exclusive with the deprecated present_layers_callback and is
  // required before FlutterEngineAddView may be used; it also serves the single
  // implicit view (id 0) unchanged.
  compositor.present_view_callback =
      [](const FlutterPresentViewInfo* info) -> bool {
    auto* self = static_cast<DrmBackend*>(info->user_data);
    return self->CompositorForView(info->view_id)
        ->PresentView(info->view_id, info->layers, info->layers_count);
  };
#else
  compositor.avoid_backing_store_cache = true;
#endif
  return compositor;
}

#if BUILD_COMPOSITOR
DrmCompositor* DrmBackend::CompositorForView(const int64_t view_id) const {
  if (const auto it = view_outputs_.find(view_id); it != view_outputs_.end()) {
    return it->second;
  }
  // Unknown view id -> the implicit view's compositor on this backend. Keeps a
  // stray present/backing-store call rendering rather than dereferencing null.
  return compositor_.get();
}

void DrmBackend::RegisterViewOutput(const int64_t view_id,
                                    DrmCompositor* compositor) {
  view_outputs_[view_id] = compositor;
}

bool DrmBackend::ResolveOutputContext(const std::string& connector_name,
                                      DrmOutputContext& out) {
  drmModeRes* res = drmModeGetResources(drm_dev_->fd());
  if (res == nullptr) {
    ihs::log::error("[DrmBackend] drmModeGetResources failed: {}",
                    std::strerror(errno));
    return false;
  }
  const auto name_of = [](const drmModeConnector* c) {
    return std::string(ConnectorTypeName(c->connector_type)) + "-" +
           std::to_string(c->connector_type_id);
  };

  drmModeConnector* connector = nullptr;
  for (int i = 0; i < res->count_connectors; ++i) {
    drmModeConnector* c =
        drmModeGetConnector(drm_dev_->fd(), res->connectors[i]);
    if (c != nullptr && c->connection == DRM_MODE_CONNECTED &&
        c->count_modes > 0 && name_of(c) == connector_name) {
      connector = c;
      break;
    }
    if (c != nullptr) {
      drmModeFreeConnector(c);
    }
  }
  if (connector == nullptr) {
    ihs::log::error(
        "[DrmBackend] additional output connector '{}' not found or not "
        "connected",
        connector_name);
    drmModeFreeResources(res);
    return false;
  }

  drmModeModeInfo mode{};
  for (int i = 0; i < connector->count_modes; ++i) {
    if ((connector->modes[i].type & DRM_MODE_TYPE_PREFERRED) != 0) {
      mode = connector->modes[i];
      break;
    }
  }
  if (mode.clock == 0) {
    mode = connector->modes[0];
  }

  // Pick a CRTC this connector can drive that no other output has claimed.
  uint32_t crtc_id = 0;
  uint32_t crtc_index = 0;
  for (int e = 0; e < connector->count_encoders && crtc_id == 0; ++e) {
    drmModeEncoder* enc =
        drmModeGetEncoder(drm_dev_->fd(), connector->encoders[e]);
    if (enc == nullptr) {
      continue;
    }
    for (int c = 0; c < res->count_crtcs; ++c) {
      if ((enc->possible_crtcs & (1u << c)) != 0 &&
          std::find(used_crtcs_.begin(), used_crtcs_.end(), res->crtcs[c]) ==
              used_crtcs_.end()) {
        crtc_id = res->crtcs[c];
        crtc_index = static_cast<uint32_t>(c);
        break;
      }
    }
    drmModeFreeEncoder(enc);
  }

  const uint32_t connector_id = connector->connector_id;
  drmModeFreeConnector(connector);
  drmModeFreeResources(res);

  if (crtc_id == 0) {
    ihs::log::error("[DrmBackend] no free CRTC for additional output '{}'",
                    connector_name);
    return false;
  }
  used_crtcs_.push_back(crtc_id);
  out = DrmOutputContext(crtc_id, crtc_index, connector_id, mode, mode.hdisplay,
                         mode.vdisplay, &resolved());
  ihs::log::info(
      "[DrmBackend] additional output '{}': connector={} crtc={} mode={}x{}@{}",
      connector_name, connector_id, crtc_id, mode.hdisplay, mode.vdisplay,
      mode.vrefresh);
  return true;
}

bool DrmBackend::AddOutput(const std::string& connector_name,
                           const int64_t view_id) {
  DrmOutputContext ctx;
  if (!ResolveOutputContext(connector_name, ctx)) {
    return false;
  }
  auto compositor = std::make_unique<DrmCompositor>(this, ctx);
  // Additional outputs do not drive the engine's single vsync (view 0 does).
  compositor->SetDrivesVsync(false);
  DrmCompositor* raw = compositor.get();
  extra_outputs_.push_back(
      ExtraOutput{view_id, std::move(compositor), ctx.width(), ctx.height()});
  view_outputs_[view_id] = raw;
  return true;
}

std::vector<DrmBackend::ViewOutputSpec> DrmBackend::AdditionalViews() const {
  std::vector<ViewOutputSpec> specs;
  specs.reserve(extra_outputs_.size());
  for (const auto& o : extra_outputs_) {
    specs.push_back(ViewOutputSpec{o.view_id, o.width, o.height});
  }
  return specs;
}
#else
bool DrmBackend::AddOutput(const std::string& /*connector_name*/,
                           int64_t /*view_id*/) {
  return false;
}

std::vector<DrmBackend::ViewOutputSpec> DrmBackend::AdditionalViews() const {
  return {};
}

void DrmBackend::RegisterViewOutput(int64_t /*view_id*/,
                                    DrmCompositor* /*compositor*/) {}
#endif

#if BUILD_COMPOSITOR
void DrmBackend::RegisterCompositorSurface(
    const FlutterPlatformViewIdentifier id,
    std::shared_ptr<ICompositorSurface> surface) {
  if (compositor_) {
    compositor_->RegisterSurface(id, std::move(surface));
  }
}

void DrmBackend::UnregisterCompositorSurface(FlutterPlatformViewIdentifier id) {
  if (compositor_) {
    compositor_->UnregisterSurface(id);
  }
}

void DrmBackend::ResizeCompositorSurface(FlutterPlatformViewIdentifier id,
                                         int32_t width,
                                         int32_t height) {
  if (compositor_) {
    compositor_->ResizeSurface(id, width, height);
  }
}
#endif

bool DrmBackend::GetEglContext(BackendEglContext* out) const {
  if (!out) {
    return false;
  }
  out->display = egl_display_;
  out->config = egl_config_;
  out->share_context = egl_context_;
  return true;
}