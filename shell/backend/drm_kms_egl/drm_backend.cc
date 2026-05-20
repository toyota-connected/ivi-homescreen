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

#include <fcntl.h>
#include <linux/vt.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "backend/drm_kms_egl/driver_probe.h"
#include "backend/drm_kms_egl/drm_compositor.h"
#include "backend/drm_kms_egl/drm_session.h"
#include "backend/gl_process_resolver.h"
#include "engine.h"
#include "logging.h"
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"

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

// Refuse to run unless our controlling terminal is the *foreground* VT on
// the kernel console. Rationale: drmSetMaster can succeed from a non-
// foreground VT (e.g. a terminal emulator, SSH, or an inactive text VT)
// when no compositor currently holds master, but the GPU keeps scanning
// out whatever the foreground VT owns. Atomic commits get silently
// accepted and no PAGE_FLIP_EVENT ever fires, so PresentLayers stalls on
// the next flip wait — the "master-acquired but black screen" pathology
// we kept hitting. Fail fast with an actionable message instead.
//
// Returns true if it's safe to proceed, false with a logged error if not.
// A missing /dev/tty0 (container / headless service) is not fatal —
// those cases are outside the scope of this check.
bool VerifyForegroundVt(const std::string& drm_device) {
  const int tty0 = ::open("/dev/tty0", O_RDONLY | O_CLOEXEC);
  if (tty0 < 0) {
    spdlog::warn(
        "[DrmBackend] open(/dev/tty0): {} — skipping foreground-VT check",
        std::strerror(errno));
    return true;
  }
  struct vt_stat vtstat{};
  const bool got_state = ::ioctl(tty0, VT_GETSTATE, &vtstat) == 0;
  const int vt_errno = errno;
  ::close(tty0);
  if (!got_state) {
    spdlog::warn("[DrmBackend] VT_GETSTATE: {} — skipping foreground-VT check",
                 std::strerror(vt_errno));
    return true;
  }
  const unsigned int active_vt = vtstat.v_active;

  // open("/dev/tty") redirects to the process's controlling terminal; fstat
  // on the resulting fd returns that terminal's device number. stat("/dev/tty")
  // would always return (5, 0) — the stats of the /dev/tty device node itself
  // — and the major check below would reject every caller, so go through the
  // open/fstat dance instead.
  const int ctl_fd = ::open("/dev/tty", O_RDONLY | O_CLOEXEC | O_NOCTTY);
  if (ctl_fd < 0) {
    spdlog::error(
        "[DrmBackend] no controlling terminal (open /dev/tty: {}). Cannot "
        "drive {} without a foreground VT — switch to the active console "
        "(tty{}) and rerun.",
        std::strerror(errno), drm_device, active_vt);
    return false;
  }
  struct stat st{};
  const int fstat_rc = ::fstat(ctl_fd, &st);
  const int fstat_errno = errno;
  ::close(ctl_fd);
  if (fstat_rc != 0) {
    spdlog::error(
        "[DrmBackend] fstat(/dev/tty): {}. Cannot drive {} without a "
        "foreground VT — switch to the active console (tty{}) and rerun.",
        std::strerror(fstat_errno), drm_device, active_vt);
    return false;
  }

  // TTY_MAJOR is 4 on Linux; /dev/ttyN lives at (4, N). Terminal emulators
  // and SSH sessions give /dev/tty a pts major (136+), which is precisely
  // the case we want to refuse.
  constexpr unsigned int kTtyMajor = 4;
  const unsigned int ctl_major = major(st.st_rdev);
  const unsigned int ctl_minor = minor(st.st_rdev);
  if (ctl_major != kTtyMajor) {
    spdlog::error(
        "[DrmBackend] controlling terminal is not a kernel VT "
        "(major={}, minor={}) — you're running from a terminal emulator or "
        "SSH. Active VT is tty{}. Switch to tty{} (Ctrl+Alt+F{}) and rerun, "
        "or `sudo systemctl isolate multi-user.target` first. Refusing to "
        "drive {}.",
        ctl_major, ctl_minor, active_vt, active_vt, active_vt, drm_device);
    return false;
  }
  if (ctl_minor != active_vt) {
    spdlog::error(
        "[DrmBackend] controlling VT is tty{} but foreground VT is tty{} — "
        "scanout goes to the foreground. Switch to tty{} (Ctrl+Alt+F{}) and "
        "rerun. Refusing to drive {}.",
        ctl_minor, active_vt, ctl_minor, ctl_minor, drm_device);
    return false;
  }
  spdlog::info("[DrmBackend] foreground VT check: on tty{} (active)",
               active_vt);
  return true;
}

const char* ConnectorTypeName(uint32_t type) {
  switch (type) {
    case DRM_MODE_CONNECTOR_Unknown:
      return "Unknown";
    case DRM_MODE_CONNECTOR_VGA:
      return "VGA";
    case DRM_MODE_CONNECTOR_DVII:
      return "DVI-I";
    case DRM_MODE_CONNECTOR_DVID:
      return "DVI-D";
    case DRM_MODE_CONNECTOR_DVIA:
      return "DVI-A";
    case DRM_MODE_CONNECTOR_Composite:
      return "Composite";
    case DRM_MODE_CONNECTOR_SVIDEO:
      return "S-Video";
    case DRM_MODE_CONNECTOR_LVDS:
      return "LVDS";
    case DRM_MODE_CONNECTOR_Component:
      return "Component";
    case DRM_MODE_CONNECTOR_9PinDIN:
      return "9PinDIN";
    case DRM_MODE_CONNECTOR_DisplayPort:
      return "DP";
    case DRM_MODE_CONNECTOR_HDMIA:
      return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB:
      return "HDMI-B";
    case DRM_MODE_CONNECTOR_TV:
      return "TV";
    case DRM_MODE_CONNECTOR_eDP:
      return "eDP";
    case DRM_MODE_CONNECTOR_VIRTUAL:
      return "Virtual";
    case DRM_MODE_CONNECTOR_DSI:
      return "DSI";
    case DRM_MODE_CONNECTOR_DPI:
      return "DPI";
    case DRM_MODE_CONNECTOR_WRITEBACK:
      return "Writeback";
    default:
      return "?";
  }
}

}  // namespace

int PrintDrmModes(const std::string& device) {
  const int fd = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    std::fprintf(stderr, "open(%s): %s\n", device.c_str(),
                 std::strerror(errno));
    return 1;
  }
  drmModeRes* res = drmModeGetResources(fd);
  if (!res) {
    std::fprintf(stderr, "drmModeGetResources(%s): %s\n", device.c_str(),
                 std::strerror(errno));
    ::close(fd);
    return 1;
  }
  std::printf("Device: %s\n", device.c_str());
  std::printf("Connectors: %d\n\n", res->count_connectors);
  for (int ci = 0; ci < res->count_connectors; ++ci) {
    drmModeConnector* c = drmModeGetConnector(fd, res->connectors[ci]);
    if (!c)
      continue;
    const char* state = (c->connection == DRM_MODE_CONNECTED) ? "connected"
                        : (c->connection == DRM_MODE_DISCONNECTED)
                            ? "disconnected"
                            : "unknown";
    std::printf("[%u] %s-%u  %s  modes=%d\n", c->connector_id,
                ConnectorTypeName(c->connector_type), c->connector_type_id,
                state, c->count_modes);
    for (int mi = 0; mi < c->count_modes; ++mi) {
      const drmModeModeInfo& m = c->modes[mi];
      const bool preferred = (m.type & DRM_MODE_TYPE_PREFERRED) != 0;
      std::printf("  %4dx%-4d @ %3dHz  %-20s  %s\n", m.hdisplay, m.vdisplay,
                  m.vrefresh, m.name, preferred ? "[preferred]" : "");
    }
    std::printf("\n");
    drmModeFreeConnector(c);
  }
  drmModeFreeResources(res);
  ::close(fd);
  return 0;
}

std::unique_ptr<DrmBackend> DrmBackend::Create(
    const DrmConfig& cfg,
    homescreen::DrmSession* session) {
  std::unique_ptr<DrmBackend> backend(new DrmBackend(cfg, session));
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
          spdlog::info(
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
  backend->compositor_ = std::make_unique<DrmCompositor>(backend.get());
#endif
  return backend;
}

DrmBackend::DrmBackend(DrmConfig cfg, homescreen::DrmSession* session)
    : cfg_(std::move(cfg)), session_(session) {}

DrmBackend::~DrmBackend() {
  // Let any in-flight page flip land so we don't free a BO still being
  // scanned out.
  (void)WaitForPendingFlip();

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

  // Reverse-watchdog no longer needed: we restored the CRTC ourselves.
  homescreen::watchdog::Disarm(drm_watchdog_);

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

  // Release DRM master so whoever takes over next (fbcon, getty, the
  // greeter coming back up) can drive the display. Must happen last,
  // after all our atomic work is done — drm::Device's destructor closes
  // the fd, which also drops master, but being explicit keeps the log
  // honest.
  if (drm_master_ && drm_dev_) {
    drmDropMaster(drm_dev_->fd());
    drm_master_ = false;
  }
  // drm_dev_ destructor closes the fd automatically.
}

bool DrmBackend::InitDrm() {
  if (session_ != nullptr) {
    // libseat path: the seat provider (logind/seatd/builtin) owns VT
    // activation and master handoff. Skip the foreground-VT check
    // (logind activates us only when the VT is ours), skip drmSetMaster
    // (libseat_open_device returns a master-capable fd while we hold
    // the seat), and skip the reverse-watchdog (the seat provider
    // releases the session cleanly on socket drop — SIGKILL included).
    const int fd = session_->TakeDevice(cfg_.drm_device);
    if (fd < 0) {
      spdlog::error("[DrmBackend] session take_device({}): failed",
                    cfg_.drm_device);
      return false;
    }
    drm_dev_.emplace(drm::Device::from_fd(fd));
    spdlog::info("[DrmBackend] opened {} via libseat (fd={})", cfg_.drm_device,
                 fd);
  } else {
    // Fallback path: no seat backend available. Refuse up-front if we're
    // not on the active VT — prevents the "master acquired, commits
    // return success, but scanout stays on the other VT" trap where
    // drmSetMaster succeeds but PAGE_FLIP_EVENT never fires.
    if (!VerifyForegroundVt(cfg_.drm_device)) {
      return false;
    }

    auto dev = drm::Device::open(cfg_.drm_device);
    if (!dev) {
      spdlog::error("[DrmBackend] open({}): {}", cfg_.drm_device,
                    dev.error().message());
      return false;
    }
    drm_dev_.emplace(std::move(*dev));

    // Acquire DRM master. Required for modeset/atomic commits. Atomic
    // writes from a non-master fd are silently accepted by some drivers
    // (amdgpu in particular) as no-ops — the commit returns success, but
    // nothing actually changes on screen, which is exactly the "blank
    // panel, no PAGE_FLIP_EVENT" pathology. Refuse to run in that state.
    if (drmSetMaster(drm_dev_->fd()) != 0) {
      if (const int err = errno;
          err == EBUSY || err == EACCES || err == EPERM) {
        spdlog::error(
            "[DrmBackend] cannot acquire DRM master on {} ({}). Another "
            "display server (gdm / gnome-shell / sddm / Xorg / a Wayland "
            "compositor) is already holding the device. Stop it or run from "
            "a bare TTY (e.g. `sudo systemctl isolate multi-user.target`).",
            cfg_.drm_device, std::strerror(err));
      } else {
        spdlog::error("[DrmBackend] drmSetMaster({}): {}", cfg_.drm_device,
                      std::strerror(err));
      }
      return false;
    }
    drm_master_ = true;
    spdlog::info("[DrmBackend] DRM master on {}", cfg_.drm_device);
  }

  if (drmSetClientCap(drm_dev_->fd(), DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) !=
      0) {
    spdlog::warn("[DrmBackend] DRM_CLIENT_CAP_UNIVERSAL_PLANES unsupported");
  }
  if (drmSetClientCap(drm_dev_->fd(), DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
    spdlog::warn("[DrmBackend] DRM_CLIENT_CAP_ATOMIC unsupported");
  }

  drmModeRes* res = drmModeGetResources(drm_dev_->fd());
  if (!res) {
    spdlog::error("[DrmBackend] drmModeGetResources failed: {}",
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
  if (cfg_.connector_name.has_value()) {
    const std::string& want = *cfg_.connector_name;
    for (drmModeConnector* c : eligible) {
      if (connector_name(c) == want) {
        connector = c;
        pick_reason = "user pin (--drm-connector)";
        break;
      }
    }
    if (!connector) {
      spdlog::error(
          "[DrmBackend] --drm-connector={} not found among eligible "
          "connectors; available:",
          want);
      for (drmModeConnector* c : eligible) {
        spdlog::error("[DrmBackend]   {}", connector_name(c));
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
    spdlog::error("[DrmBackend] no connected connector found");
    drmModeFreeResources(res);
    return false;
  }
  for (drmModeConnector* c : eligible) {
    if (c != connector) {
      drmModeFreeConnector(c);
    }
  }
  spdlog::info("[DrmBackend] picked connector {} via {}",
               connector_name(connector), pick_reason);
  connector_id_ = connector->connector_id;

  // Always drive the display at its preferred/native mode. If the user
  // specified a (smaller) size, we keep the preferred mode for the CRTC
  // and letterbox the requested FB inside it (see fb_w_/fb_h_ below).
  // Picking a smaller mode to match the request is worse on almost every
  // axis — blurry on LCDs, worse backlight uniformity on some panels,
  // and on some connectors the mode list doesn't contain an exact match.
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
    spdlog::error("[DrmBackend] no CRTC available for connector {}",
                  connector_id_);
    drmModeFreeConnector(connector);
    drmModeFreeResources(res);
    return false;
  }

  saved_crtc_ = drmModeGetCrtc(drm_dev_->fd(), crtc_id_);

  if (fb_w_ == mode_.hdisplay && fb_h_ == mode_.vdisplay) {
    spdlog::info("[DrmBackend] connector={} crtc={} mode={}x{}@{}Hz",
                 connector_id_, crtc_id_, mode_.hdisplay, mode_.vdisplay,
                 mode_.vrefresh);
  } else {
    spdlog::info(
        "[DrmBackend] connector={} crtc={} mode={}x{}@{}Hz fb={}x{} "
        "(letterboxed)",
        connector_id_, crtc_id_, mode_.hdisplay, mode_.vdisplay, mode_.vrefresh,
        fb_w_, fb_h_);
  }

  // Arm the reverse-watchdog BEFORE any code path can call drmModeSetCrtc
  // (the first one lands in SetInitialMode via the first Present). If the
  // parent dies — SIGKILL included — the child restores this snapshot, so
  // the text console comes back instead of the last Flutter framebuffer.
  //
  // When a seat session is live, skip it: the seat provider (logind/seatd)
  // observes our socket close on SIGKILL and releases the session itself,
  // which restores the underlying TTY fb. The reverse watchdog would be
  // redundant (and its inherited fd could briefly fight the seat
  // provider's cleanup).
  if (saved_crtc_ && session_ == nullptr) {
    drm_watchdog_ = homescreen::watchdog::SpawnDrmRestore(
        drm_dev_->fd(), saved_crtc_->crtc_id, saved_crtc_->buffer_id,
        saved_crtc_->x, saved_crtc_->y, connector_id_, saved_crtc_->mode);
  }

  drmModeFreeConnector(connector);
  drmModeFreeResources(res);
  return true;
}

bool DrmBackend::InitGbm() {
  // DriverProbe returns 0 when the primary plane advertises none of the
  // formats we know how to drive; surface that clearly instead of letting
  // gbm_surface_create fail with a generic nullptr.
  if (resolved_->primary_format == 0) {
    spdlog::error(
        "[DrmBackend] primary plane advertises no supported format "
        "(XRGB8888/XBGR8888/ARGB8888/ABGR8888/RGB565). Override with "
        "--drm-primary-format if you know what's there.");
    return false;
  }

  gbm_device_ = gbm_create_device(drm_dev_->fd());
  if (!gbm_device_) {
    spdlog::error("[DrmBackend] gbm_create_device failed");
    return false;
  }

  gbm_surface_ =
      gbm_surface_create(gbm_device_, fb_w_, fb_h_, resolved_->primary_format,
                         GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!gbm_surface_) {
    spdlog::error("[DrmBackend] gbm_surface_create(format=0x{:08x}) failed",
                  resolved_->primary_format);
    return false;
  }
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
    spdlog::error("[DrmBackend] eglGetPlatformDisplay failed");
    return false;
  }

  EGLint major = 0;
  EGLint minor = 0;
  if (!eglInitialize(egl_display_, &major, &minor)) {
    spdlog::error("[DrmBackend] eglInitialize failed: 0x{:x}", eglGetError());
    return false;
  }
  SPDLOG_DEBUG("[DrmBackend] EGL {}.{}", major, minor);

  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    spdlog::error("[DrmBackend] eglBindAPI failed");
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
    spdlog::error("[DrmBackend] eglChooseConfig (count) failed: 0x{:x}",
                  eglGetError());
    return false;
  }
  std::vector<EGLConfig> configs(static_cast<size_t>(num_configs));
  if (!eglChooseConfig(egl_display_, egl_attrs, configs.data(), num_configs,
                       &num_configs)) {
    spdlog::error("[DrmBackend] eglChooseConfig (fill) failed: 0x{:x}",
                  eglGetError());
    return false;
  }

  auto attr = [this](EGLConfig c, EGLint id) -> EGLint {
    EGLint v = 0;
    eglGetConfigAttrib(egl_display_, c, id, &v);
    return v;
  };

  if (cfg_.debug_backend) {
    spdlog::info("[DrmBackend] {} candidate EGL configs", num_configs);
    for (EGLint i = 0; i < num_configs; ++i) {
      spdlog::info(
          "[DrmBackend]   [{}] visual=0x{:08x} R{}G{}B{}A{} depth={} "
          "stencil={} samples={} caveat=0x{:x} renderable=0x{:x} "
          "surface=0x{:x} conformant=0x{:x}",
          i, static_cast<unsigned>(attr(configs[i], EGL_NATIVE_VISUAL_ID)),
          attr(configs[i], EGL_RED_SIZE), attr(configs[i], EGL_GREEN_SIZE),
          attr(configs[i], EGL_BLUE_SIZE), attr(configs[i], EGL_ALPHA_SIZE),
          attr(configs[i], EGL_DEPTH_SIZE), attr(configs[i], EGL_STENCIL_SIZE),
          attr(configs[i], EGL_SAMPLES),
          static_cast<unsigned>(attr(configs[i], EGL_CONFIG_CAVEAT)),
          static_cast<unsigned>(attr(configs[i], EGL_RENDERABLE_TYPE)),
          static_cast<unsigned>(attr(configs[i], EGL_SURFACE_TYPE)),
          static_cast<unsigned>(attr(configs[i], EGL_CONFORMANT)));
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
    return {
        attr(configs[i], EGL_SAMPLES),                                    // 1
        attr(configs[i], EGL_CONFIG_CAVEAT) == EGL_NONE ? 0 : 1,          // 2
        abs_diff(attr(configs[i], EGL_ALPHA_SIZE), preferred_alpha),      // 3
        abs_diff(attr(configs[i], EGL_STENCIL_SIZE), kPreferredStencil),  // 4
        attr(configs[i], EGL_DEPTH_SIZE),                                 // 5
    };
  };

  egl_config_ = nullptr;
  EGLint best_idx = -1;
  for (EGLint i = 0; i < num_configs; ++i) {
    if (attr(configs[i], EGL_NATIVE_VISUAL_ID) !=
        static_cast<EGLint>(resolved_->primary_format)) {
      continue;
    }
    if (best_idx < 0 || score(i) < score(best_idx)) {
      best_idx = i;
    }
  }
  if (best_idx < 0) {
    spdlog::error(
        "[DrmBackend] no EGL config matches GBM format 0x{:x} (checked {} "
        "configs)",
        resolved_->primary_format, num_configs);
    return false;
  }
  egl_config_ = configs[static_cast<size_t>(best_idx)];
  if (cfg_.debug_backend) {
    spdlog::info("[DrmBackend] selected config [{}] (compositor={})", best_idx,
                 BUILD_COMPOSITOR ? "on" : "off");
  }

  egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT,
                                  kEsContextAttribs.data());
  if (egl_context_ == EGL_NO_CONTEXT) {
    spdlog::error("[DrmBackend] eglCreateContext failed: 0x{:x}",
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
    spdlog::error("[DrmBackend] eglCreateWindowSurface failed: 0x{:x}",
                  eglGetError());
    return false;
  }
  return true;
}

uint32_t DrmBackend::AddFb(gbm_bo* bo) const {
  const uint32_t width = gbm_bo_get_width(bo);
  const uint32_t height = gbm_bo_get_height(bo);
  const uint32_t stride = gbm_bo_get_stride(bo);
  const uint32_t handle = gbm_bo_get_handle(bo).u32;
  uint32_t fb_id = 0;
  if (drmModeAddFB(drm_dev_->fd(), width, height, 24, 32, stride, handle,
                   &fb_id) != 0) {
    spdlog::error("[DrmBackend] drmModeAddFB: {}", std::strerror(errno));
    return 0;
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
    spdlog::error(
        "[DrmBackend] legacy modeset reached with framed FB ({}x{} on "
        "{}x{} mode); atomic plane-compositor must succeed for framing. "
        "Remove view.width/view.height from config to run full-screen.",
        fb_w_, fb_h_, mode_.hdisplay, mode_.vdisplay);
    return false;
  }
  if (drmModeSetCrtc(drm_dev_->fd(), crtc_id_, current_fb_, 0, 0,
                     &connector_id_, 1, &mode_) != 0) {
    spdlog::error("[DrmBackend] drmModeSetCrtc: {}", std::strerror(errno));
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
  if (!cfg_.debug_backend) {
    return;
  }
  const uint64_t now = LibFlutterEngine->GetCurrentTime();

  if (flip_submit_ns_ != 0) {
    const double latency_ms = static_cast<double>(now - flip_submit_ns_) / 1e6;
    SPDLOG_DEBUG("[DrmBackend] flip latency: {:.2f} ms", latency_ms);
    flip_submit_ns_ = 0;
  }

  ++frame_count_;
  if (fps_epoch_ns_ == 0) {
    fps_epoch_ns_ = now;
  }
  if (const double elapsed_s = static_cast<double>(now - fps_epoch_ns_) / 1e9;
      elapsed_s >= 1.0) {
    spdlog::info("[DrmBackend] FPS: {:.1f} ({} frames / {:.2f}s)",
                 frame_count_ / elapsed_s, frame_count_, elapsed_s);
    frame_count_ = 0;
    fps_epoch_ns_ = now;
  }
}

void DrmBackend::SetVsyncBaton(FLUTTER_API_SYMBOL(FlutterEngine) engine,
                               const intptr_t baton) {
  // Bootstrap: before the first page-flip, there's no flip-complete event
  // to return the baton from. Return it immediately so Flutter can
  // schedule its first frame. Subsequent batons go through the normal
  // PageFlipHandler path, locked to actual vblank.
  if (!mode_set_) {
    const uint64_t now = LibFlutterEngine->GetCurrentTime();
    const uint64_t period_ns =
        mode_.vrefresh > 0
            ? 1000000000ULL / static_cast<uint64_t>(mode_.vrefresh)
            : 16666667ULL;
    LibFlutterEngine->OnVsync(engine, baton, now, now + period_ns);
    return;
  }
  vsync_engine_.store(engine, std::memory_order_relaxed);
  vsync_baton_.store(baton, std::memory_order_release);
}

void DrmBackend::PageFlipHandler(int /*fd*/,
                                 unsigned int /*sequence*/,
                                 unsigned int /*tv_sec*/,
                                 unsigned int /*tv_usec*/,
                                 void* user_data) {
  auto* self = static_cast<DrmBackend*>(user_data);
  // The page flip just promoted pending → scanout. What was scanned out
  // before is now safe to release.
  if (self->current_fb_ != 0) {
    drmModeRmFB(self->drm_dev_->fd(), self->current_fb_);
  }
  if (self->current_bo_) {
    gbm_surface_release_buffer(self->gbm_surface_, self->current_bo_);
  }
  self->current_bo_ = self->pending_bo_;
  self->current_fb_ = self->pending_fb_;
  self->pending_bo_ = nullptr;
  self->pending_fb_ = 0;
  self->flip_pending_ = false;
  self->RecordFlipComplete();

  // Return the vsync baton to the engine if one is pending. This tells
  // Flutter's scheduler "the display just vblanked — start the next
  // frame now, targeting the subsequent vblank."
  const intptr_t baton =
      self->vsync_baton_.exchange(0, std::memory_order_acq_rel);
  if (baton != 0) {
    if (const auto engine =
            self->vsync_engine_.load(std::memory_order_relaxed)) {
      const uint64_t now = LibFlutterEngine->GetCurrentTime();
      const uint64_t period_ns =
          self->mode_.vrefresh > 0
              ? 1000000000ULL / static_cast<uint64_t>(self->mode_.vrefresh)
              : 16666667ULL;
      LibFlutterEngine->OnVsync(engine, baton, now, now + period_ns);
    }
  }
}

bool DrmBackend::WaitForPendingFlip() const {
  if (!flip_pending_ || !drm_dev_) {
    return true;
  }

  drmEventContext ctx{};
  ctx.version = 2;
  ctx.page_flip_handler = &DrmBackend::PageFlipHandler;

  while (flip_pending_) {
    pollfd pfd{};
    pfd.fd = drm_dev_->fd();
    pfd.events = POLLIN;
    if (const int r = poll(&pfd, 1, -1); r < 0) {
      if (errno == EINTR) {
        continue;
      }
      spdlog::error("[DrmBackend] poll: {}", std::strerror(errno));
      return false;
    }
    if (pfd.revents & POLLIN) {
      drmHandleEvent(drm_dev_->fd(), &ctx);
    }
  }
  return true;
}

bool DrmBackend::Present() {
  if (!WaitForPendingFlip()) {
    return false;
  }

  if (!eglSwapBuffers(egl_display_, egl_surface_)) {
    spdlog::error("[DrmBackend] eglSwapBuffers: 0x{:x}", eglGetError());
    return false;
  }

  gbm_bo* next_bo = gbm_surface_lock_front_buffer(gbm_surface_);
  if (!next_bo) {
    spdlog::error("[DrmBackend] gbm_surface_lock_front_buffer failed");
    return false;
  }

  const uint32_t next_fb = AddFb(next_bo);
  if (next_fb == 0) {
    gbm_surface_release_buffer(gbm_surface_, next_bo);
    return false;
  }

  if (!mode_set_) {
    current_bo_ = next_bo;
    current_fb_ = next_fb;
    if (!SetInitialMode()) {
      return false;
    }
    RecordFlipComplete();
    return true;
  }

  if (cfg_.debug_backend) {
    flip_submit_ns_ = LibFlutterEngine->GetCurrentTime();
  }
  if (drmModePageFlip(drm_dev_->fd(), crtc_id_, next_fb,
                      DRM_MODE_PAGE_FLIP_EVENT, this) != 0) {
    spdlog::warn("[DrmBackend] drmModePageFlip: {}", std::strerror(errno));
    drmModeRmFB(drm_dev_->fd(), next_fb);
    gbm_surface_release_buffer(gbm_surface_, next_bo);
    return false;
  }

  // The kernel now owns next_bo/next_fb until the page-flip-complete event
  // fires. current_bo_/current_fb_ remain the live scanout until then.
  pending_bo_ = next_bo;
  pending_fb_ = next_fb;
  flip_pending_ = true;
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
    return static_cast<DrmBackend*>(user_data)->compositor_->CreateBackingStore(
        config, out);
  };
  compositor.collect_backing_store_callback =
      [](const FlutterBackingStore* store, void* user_data) -> bool {
    return static_cast<DrmBackend*>(user_data)
        ->compositor_->CollectBackingStore(store);
  };
  compositor.present_layers_callback =
      [](const FlutterLayer** layers, size_t count, void* user_data) -> bool {
    return static_cast<DrmBackend*>(user_data)->compositor_->PresentLayers(
        layers, count);
  };
#else
  compositor.avoid_backing_store_cache = true;
#endif
  return compositor;
}

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