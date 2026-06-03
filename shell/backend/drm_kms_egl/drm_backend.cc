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

#include "asio/post.hpp"
#include "backend/drm_kms_egl/drm_capture.h"
#include "backend/drm_kms_egl/drm_compositor.h"
#include "backend/drm_kms_egl/drm_cursor.h"
#include "backend/drm_kms_egl/drm_session.h"
#include "backend/gl_process_resolver.h"
#include "engine.h"
#include "logging.h"
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

  // open("/dev/tty") redirects to the process's controlling terminal, so
  // calling open() against the special inode is the only way to acquire
  // an fd that *targets* the ctty rather than the /dev/tty special device
  // itself. ENXIO from open() means the process has no ctty at all (e.g.
  // a daemon without setsid + TIOCSCTTY).
  const int ctl_fd = ::open("/dev/tty", O_RDONLY | O_CLOEXEC | O_NOCTTY);
  if (ctl_fd < 0) {
    spdlog::error(
        "[DrmBackend] no controlling terminal (open /dev/tty: {}). Cannot "
        "drive {} without a foreground VT — switch to the active console "
        "(tty{}) and rerun.",
        std::strerror(errno), drm_device, active_vt);
    return false;
  }
  // fstat() of the resulting fd does NOT return the underlying tty's
  // device numbers — it returns /dev/tty's own (5, 0) inode metadata on
  // every Linux kernel from 4.x onward. The earlier version of this
  // check believed fstat would resolve through; in practice it
  // mis-rejected every legitimate kernel-VT ctty too, including
  // systemd-launched services attached via TTYPath=/dev/tty1. TIOCGDEV
  // is the canonical ioctl for retrieving the actual tty device number
  // from a /dev/tty fd; available since Linux 2.6.18.
  unsigned int dev = 0;
  const int ioctl_rc = ::ioctl(ctl_fd, TIOCGDEV, &dev);
  const int ioctl_errno = errno;
  ::close(ctl_fd);
  if (ioctl_rc != 0) {
    spdlog::error(
        "[DrmBackend] TIOCGDEV(/dev/tty): {}. Cannot drive {} without "
        "a foreground VT — switch to the active console (tty{}) and rerun.",
        std::strerror(ioctl_errno), drm_device, active_vt);
    return false;
  }

  // TTY_MAJOR is 4 on Linux; /dev/ttyN lives at (4, N). Terminal emulators
  // and SSH sessions give /dev/tty a pts major (136+), which is precisely
  // the case we want to refuse.
  constexpr unsigned int kTtyMajor = 4;
  const unsigned int ctl_major = major(static_cast<dev_t>(dev));
  const unsigned int ctl_minor = minor(static_cast<dev_t>(dev));
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

  // HW cursor on the CRTC's cursor plane (or legacy drmModeSetCursor
  // when the driver doesn't expose one). Failure is non-fatal —
  // pointer events still flow to Flutter, just without a visible
  // sprite. Independent of the compositor: cursor commits run on
  // their own AtomicRequests on the seat dispatch thread.
#if HAVE_DRM_CURSOR
  backend->cursor_ = homescreen::DrmCursor::Create(
      *backend->drm_dev_, backend->crtc_id_, backend->connector_id_,
      backend->mode_, backend->fb_w_, backend->fb_h_);
#if BUILD_COMPOSITOR
  // On a CRTC with no dedicated cursor plane the cursor takes an
  // overlay; reserve it so the scene allocator's disable-unused pass
  // doesn't toggle it off every commit (flicker on pointer motion).
  // plane_id()==0 (legacy cursor path) is a no-op in the compositor.
  if (backend->cursor_ && backend->compositor_) {
    backend->compositor_->ReserveCursorPlane(backend->cursor_->plane_id());
  }
#endif
#endif
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
// spdlog logger so they interleave correctly with the embedder's own
// output (and respect spdlog's level / sink config) instead of going
// to stderr via drm-cxx's default print sink.
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
          spdlog::error("[drm-cxx] {}", m);
          break;
        case drm::LogLevel::Warn:
          spdlog::warn("[drm-cxx] {}", m);
          break;
        case drm::LogLevel::Info:
          spdlog::info("[drm-cxx] {}", m);
          break;
        case drm::LogLevel::Debug:
          spdlog::debug("[drm-cxx] {}", m);
          break;
        case drm::LogLevel::Silent:
          break;
      }
    });
  });
}
}  // namespace

DrmBackend::DrmBackend(const DrmConfig& cfg, homescreen::DrmSession* session)
    : cfg_(cfg), session_(session) {
  InstallDrmCxxLogSink();
}

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
    // (logind activates us only when the VT is ours) and drmSetMaster
    // (libseat_open_device returns a master-capable fd while we hold
    // the seat). The seat provider also releases the session cleanly
    // on socket drop (SIGKILL included), restoring TTY state externally.
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
      spdlog::error(
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
      spdlog::error(
          "[DrmBackend] --drm-mode='{}' not available on connector {}; run "
          "--drm-list-modes for the supported list",
          *cfg_.mode_spec, connector_name(connector));
      drmModeFreeConnector(connector);
      drmModeFreeResources(res);
      return false;
    }
    spdlog::info("[DrmBackend] mode override: {}x{}@{}Hz (--drm-mode)",
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
        spdlog::info(
            "[DrmBackend] gbm_surface_create_with_modifiers OK "
            "(format={}, {} IN_FORMATS modifiers)",
            drm::format_name(resolved_->primary_format), modifiers.size());
        return true;
      }
      spdlog::debug(
          "[DrmBackend] gbm_surface_create_with_modifiers failed "
          "(errno={}); falling back to legacy gbm_surface_create",
          errno);
    }
  }

  gbm_surface_ =
      gbm_surface_create(gbm_device_, fb_w_, fb_h_, resolved_->primary_format,
                         GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!gbm_surface_) {
    spdlog::error("[DrmBackend] gbm_surface_create(format={}) failed",
                  drm::format_name(resolved_->primary_format));
    return false;
  }
  spdlog::info("[DrmBackend] gbm_surface_create OK (format={}, legacy path)",
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
      EGLConfig c = configs[static_cast<size_t>(i)];
      spdlog::info(
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
      spdlog::error(
          "[DrmBackend] drmModeAddFB2WithModifiers({}x{}, fmt=0x{:08x}, "
          "mod=0x{:016x}, planes={}): {}",
          width, height, format, modifier, plane_count, std::strerror(errno));
      return 0;
    }
  } else if (drmModeAddFB2(drm_dev_->fd(), width, height, format, handles,
                           pitches, offsets, &fb_id, 0) != 0) {
    spdlog::error("[DrmBackend] drmModeAddFB2({}x{}, fmt=0x{:08x}, linear): {}",
                  width, height, format, std::strerror(errno));
    return 0;
  }
  if (cfg_.debug_backend) {
    spdlog::debug(
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
    [[maybe_unused]] const double latency_ms =
        static_cast<double>(now - flip_submit_ns_) / 1e6;
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

void DrmBackend::PostOnVsync(FLUTTER_API_SYMBOL(FlutterEngine) engine,
                             const intptr_t baton) const {
  if (engine == nullptr || baton == 0) {
    return;
  }
  auto* runner = platform_task_runner_.load(std::memory_order_acquire);
  if (runner == nullptr || runner->GetStrandContext() == nullptr) {
    // Runner not wired yet (the engine's first vsync request raced
    // SetPlatformTaskRunner during bring-up). Callers leave the baton
    // parked in vsync_baton_ in this case rather than exchanging it out,
    // so SetPlatformTaskRunner's kick latch delivers it once the runner
    // is wired (#210).
    return;
  }
  const uint64_t now = LibFlutterEngine->GetCurrentTime();
  const uint64_t period_ns =
      mode_.vrefresh > 0 ? 1000000000ULL / static_cast<uint64_t>(mode_.vrefresh)
                         : 16666667ULL;
  asio::post(*runner->GetStrandContext(), [engine, baton, now, period_ns]() {
    LibFlutterEngine->OnVsync(engine, baton, now, now + period_ns);
  });
}

void DrmBackend::SetVsyncBaton(FLUTTER_API_SYMBOL(FlutterEngine) engine,
                               const intptr_t baton) {
  engine_handle_.store(engine, std::memory_order_release);

  // Store the baton first so that if the asio flip monitor races us
  // (PAGE_FLIP_EVENT arrives between our flip-pending check and
  // store), it picks up the baton and we don't double-deliver.
  vsync_baton_.store(baton, std::memory_order_release);

  // If a flip is in flight, the asio handler will fire OnVsync when
  // PAGE_FLIP_EVENT lands. If not, the pipeline is idle (first frame,
  // post-resume, or Flutter waking from idle on input) and there's
  // nothing to feed the asio handler — we have to kick the baton
  // ourselves or Flutter will sit forever waiting for OnVsync that
  // never comes. Check both flip latches: backend's is set by the
  // legacy Present, compositor's is set by atomic commits.
  bool flip_in_flight = flip_pending_.load(std::memory_order_acquire);
#if BUILD_COMPOSITOR
  if (compositor_ && compositor_->planes_active()) {
    flip_in_flight = compositor_->IsFlipPending();
  }
#endif
  if (flip_in_flight) {
    return;
  }

  // Idle pipeline — kick the baton inline. Only pull it out of
  // vsync_baton_ if the platform task runner is actually wired and can
  // deliver it; otherwise leave it parked so SetPlatformTaskRunner's kick
  // latch delivers it once wired. At cold start the engine's first vsync
  // request can land before SetPlatformTaskRunner; exchanging the baton
  // out only to drop it in PostOnVsync stranded Flutter's first frame
  // (#210) — frame 1 was never produced (black screen + only the HW
  // cursor). Gating here (rather than exchange-then-restore) keeps the
  // hand-off to the kick latch race-free: the baton is taken exactly once,
  // by whichever of this path / the latch first sees the runner wired.
  auto* runner = platform_task_runner_.load(std::memory_order_acquire);
  if (runner == nullptr || runner->GetStrandContext() == nullptr) {
    return;  // parked in vsync_baton_; SetPlatformTaskRunner drains it
  }
  if (const intptr_t mine = vsync_baton_.exchange(0, std::memory_order_acq_rel);
      mine != 0) {
    PostOnVsync(engine, mine);
  }
}

void DrmBackend::OnSessionPaused() {
  spdlog::info("[DrmBackend] session paused (VT switch-out)");
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
    spdlog::error(
        "[DrmBackend] resume: fd changed ({} -> {}) — preserve-fd contract "
        "violated; per-fd state (FB ids, EGL/GBM, property cache) is stale "
        "and the next commit will likely fail",
        drm_dev_->fd(), new_fd);
  }
  spdlog::info("[DrmBackend] session resumed (VT switch-in, fd={})", new_fd);
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
    spdlog::warn(
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
  if (const intptr_t pending =
          vsync_baton_.exchange(0, std::memory_order_acq_rel);
      pending != 0) {
    PostOnVsync(engine, pending);
    spdlog::info("[DrmBackend] resume: drained pending baton via PostOnVsync");
  } else {
    const FlutterEngineResult r = LibFlutterEngine->ScheduleFrame(engine);
    if (r != kSuccess) {
      spdlog::warn("[DrmBackend] resume: ScheduleFrame failed (rc={})",
                   static_cast<int>(r));
    } else {
      spdlog::info("[DrmBackend] resume: ScheduleFrame requested (idle UI)");
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
                                        unsigned int /*tv_sec*/,
                                        unsigned int /*tv_usec*/,
                                        void* user_data) {
  // Always cast to DrmBackend* — both legacy Present and compositor
  // commits register `this`/`backend_` as user_data with that type.
  auto* self = static_cast<DrmBackend*>(user_data);
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
    spdlog::info("[DrmBackend] flip #{} handled (asio monitor)", n);
  }
#if BUILD_COMPOSITOR
  // Dispatch the completion to whichever subsystem ARMED this flip, identified
  // by its pending flag — NOT by planes_active(). planes_active() reflects who
  // owns scanout in steady state, but when the compositor is active and a frame
  // falls back to backend_->Present()'s legacy page flip (e.g. a backing-store
  // layer needs composition), the completion must clear
  // DrmBackend::flip_pending_. Routing by planes_active() cleared the
  // compositor's (unset) flag instead, leaving DrmBackend::flip_pending_ stuck
  // and deadlocking the next WaitForPendingFlip(). At most one flip is in
  // flight per CRTC, so at most one flag is set; the compositor's pending flag
  // therefore identifies its own flips.
  if (self->compositor_ && self->compositor_->IsFlipPending()) {
    self->compositor_->OnFlipComplete();
  } else {
    self->OnLegacyFlipComplete();
  }
#else
  self->OnLegacyFlipComplete();
#endif

  // Return the vsync baton to the engine if one is pending. We're on
  // the platform task runner thread already (asio dispatched the
  // async_wait completion here), so OnVsync can be called directly —
  // no PostOnVsync round-trip needed.
  const intptr_t baton =
      self->vsync_baton_.exchange(0, std::memory_order_acq_rel);
  if (baton == 0) {
    return;
  }
  const auto engine = self->engine_handle_.load(std::memory_order_acquire);
  if (engine == nullptr) {
    return;
  }
  const uint64_t now = LibFlutterEngine->GetCurrentTime();
  const uint64_t period_ns =
      self->mode_.vrefresh > 0
          ? 1000000000ULL / static_cast<uint64_t>(self->mode_.vrefresh)
          : 16666667ULL;
  LibFlutterEngine->OnVsync(engine, baton, now, now + period_ns);
}

void DrmBackend::SetPlatformTaskRunner(TaskRunner* runner) {
  platform_task_runner_.store(runner, std::memory_order_release);
  StartFlipMonitor();

  // Kick latch (#210). The engine may have requested its first vsync
  // before this runner was wired (cold-start race between Engine::Run and
  // this call). In that case SetVsyncBaton left the baton parked in
  // vsync_baton_ rather than dropping it. Deliver it now so Flutter
  // renders frame 1. Storing the runner above (release) before this drain
  // (acq_rel exchange) makes the hand-off exact: SetVsyncBaton either saw
  // the runner wired and took the baton itself, or saw it unwired and
  // parked the baton — in which case its release-store of the baton
  // happens-before this exchange, so we observe and deliver it here. Without
  // this latch the first baton is never answered and the app hangs on a
  // black screen (only the HW cursor visible) until restart.
  if (auto* engine = engine_handle_.load(std::memory_order_acquire);
      engine != nullptr) {
    if (const intptr_t baton =
            vsync_baton_.exchange(0, std::memory_order_acq_rel);
        baton != 0) {
      PostOnVsync(engine, baton);
    }
  }
}

void DrmBackend::StartFlipMonitor() {
  if (flip_descriptor_.has_value()) {
    return;  // already started
  }
  auto* runner = platform_task_runner_.load(std::memory_order_acquire);
  if (runner == nullptr || runner->GetIoContext() == nullptr ||
      !drm_dev_.has_value()) {
    return;
  }
  // assign() makes asio take ownership of the fd — StopVsyncMonitor must
  // call release() before reset() so drm_dev_'s close on its own fd
  // isn't a double-close.
  flip_descriptor_.emplace(*runner->GetIoContext());
  flip_descriptor_->assign(drm_dev_->fd());
  ArmFlipRead();
  spdlog::info("[DrmBackend] flip monitor armed on fd={}", drm_dev_->fd());
}

void DrmBackend::StopVsyncMonitor() {
  if (flip_descriptor_.has_value()) {
    std::error_code ec;
    // cancel() wakes the worker thread out of epoll_wait — the pending
    // async_wait completion fires with operation_aborted, asio's
    // outstanding-work count drops to zero, and the TaskRunner worker
    // loop's run_one() can finally return.
    flip_descriptor_->cancel(ec);
    // release() detaches the fd so the descriptor's destructor doesn't
    // close it (drm_dev_ owns the fd lifetime). Takes no args, returns
    // the native handle which we deliberately discard — drm_dev_ closes
    // it later.
    (void)flip_descriptor_->release();
    flip_descriptor_.reset();
  }
  platform_task_runner_.store(nullptr, std::memory_order_release);

  // Synchronously drain any in-flight PAGE_FLIP_EVENT. Without this,
  // ~DrmCompositor → WaitForPendingFlip spins forever: the async flip
  // monitor was what called drmHandleEvent to clear flip_pending_, and
  // we just shut it down. The kernel typically fires the event within
  // one vblank period (~16ms at 60Hz); poll briefly with a margin and
  // dispatch via the same UnifiedPageFlipHandler the async path used.
  // If the flag is still set after the poll loop bails (commit was
  // dropped, session paused, etc.), force-clear it so destructors can
  // make progress instead of hanging.
  if (drm_dev_.has_value() && flip_pending_.load(std::memory_order_acquire)) {
    constexpr int kDrainTimeoutMs = 100;
    constexpr int kPollSliceMs = 10;
    int elapsed_ms = 0;
    while (elapsed_ms < kDrainTimeoutMs &&
           flip_pending_.load(std::memory_order_acquire)) {
      pollfd pfd{drm_dev_->fd(), POLLIN, 0};
      const int rc = ::poll(&pfd, 1, kPollSliceMs);
      if (rc > 0 && (pfd.revents & POLLIN)) {
        drmEventContext ctx{};
        ctx.version = 2;
        ctx.page_flip_handler = &DrmBackend::UnifiedPageFlipHandler;
        drmHandleEvent(drm_dev_->fd(), &ctx);
      } else if (rc < 0 && errno != EINTR) {
        break;
      }
      elapsed_ms += kPollSliceMs;
    }
    if (flip_pending_.load(std::memory_order_acquire)) {
      spdlog::warn(
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

void DrmBackend::ArmFlipRead() {
  if (!flip_descriptor_.has_value()) {
    return;
  }
  flip_descriptor_->async_wait(
      asio::posix::stream_descriptor::wait_read,
      [this](const std::error_code& ec) {
        if (ec) {
          // operation_aborted fires on shutdown/cancellation; quiet path.
          if (ec != asio::error::operation_aborted) {
            spdlog::warn("[DrmBackend] flip monitor wait: {}", ec.message());
          }
          return;
        }
        if (!drm_dev_.has_value()) {
          return;
        }
        drmEventContext ctx{};
        ctx.version = 2;
        ctx.page_flip_handler = &DrmBackend::UnifiedPageFlipHandler;
        drmHandleEvent(drm_dev_->fd(), &ctx);
        ArmFlipRead();  // re-arm for the next vblank
      });
}

bool DrmBackend::WaitForPendingFlip() const {
  // The asio flip monitor (StartFlipMonitor) drains PAGE_FLIP_EVENTs
  // on the platform task runner thread and clears flip_pending_ via
  // UnifiedPageFlipHandler → OnLegacyFlipComplete. We just wait for
  // it. Short sleep so a 60 Hz frame's ~16ms window has plenty of
  // resolution but we don't burn the rasterizer CPU.
  using namespace std::chrono_literals;
  // Bounded wait. On nvidia-drm a flip's PAGE_FLIP_EVENT can go
  // undelivered, and at teardown the asio monitor that would drain it
  // is already gone — an unbounded loop then spins forever in
  // hrtimer_nanosleep, hanging Present and (fatally) ~DrmBackend so the
  // process never exits on SIGTERM. Cap the wait and proceed; a healthy
  // 60Hz flip clears in ~16ms so this never fires in normal operation.
  // Same budget as StopVsyncMonitor's drain.
  constexpr auto kFlipWaitTimeout = 100ms;
  const auto deadline = std::chrono::steady_clock::now() + kFlipWaitTimeout;
  while (flip_pending_.load(std::memory_order_acquire)) {
    if (!drm_dev_) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      spdlog::warn(
          "[DrmBackend] WaitForPendingFlip: no flip completion after 100ms; "
          "proceeding (PAGE_FLIP_EVENT likely lost)");
      return true;
    }
    std::this_thread::sleep_for(500us);
  }
  return true;
}

bool DrmBackend::Present() {
  MaybeCaptureSnapshot();
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
    // Mirror the compositor's first-commit drain: drmModeSetCrtc
    // produces no PAGE_FLIP_EVENT, so the second baton (Flutter
    // requested it after the first OnVsync) would sit in vsync_baton_
    // unfired. Drain it now via the platform task runner; subsequent
    // batons go through the normal PageFlipHandler path.
    if (auto* engine = engine_handle_.load(std::memory_order_acquire);
        engine != nullptr) {
      if (const intptr_t baton =
              vsync_baton_.exchange(0, std::memory_order_acq_rel);
          baton != 0) {
        PostOnVsync(engine, baton);
      }
    }
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