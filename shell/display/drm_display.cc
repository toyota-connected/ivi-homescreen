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

#include "display/drm_display.h"
#include "logging/logging.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <optional>
#include <utility>

#include <fcntl.h>
#include <linux/vt.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <drm-cxx/core/device.hpp>

#include "asio/error.hpp"

#include "backend/drm_kms_egl/drm_session.h"
#include "cursor_kind.h"
#include "display/icursor_shape_sink.h"
#include "input/drm_seat.h"
#include "logging.h"

namespace {

std::unique_ptr<homescreen::DrmSession> OpenSessionOrLog(bool no_seat) {
  if (no_seat) {
    ihs::log::warn(
        "[DrmDisplay] --drm-no-seat: bypassing libseat — the backend opens "
        "/dev/dri + input directly and self-acquires DRM master, skipping "
        "the foreground-VT guard. No VT-switch / pause-resume; you are "
        "responsible for ensuring nothing else holds the display.");
    return nullptr;
  }
  auto session = homescreen::DrmSession::Open();
  if (!session) {
    ihs::log::info(
        "[DrmDisplay] no libseat backend available — falling back to "
        "direct /dev/dri open + legacy VT/master guards");
  } else {
    ihs::log::info("[DrmDisplay] libseat session active");
  }
  return session;
}

drm::input::InputDeviceOpener OpenerFrom(homescreen::DrmSession* session) {
  if (session == nullptr) {
    return {};
  }
  return session->InputOpener();
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
    ihs::log::warn(
        "[DrmDisplay] open(/dev/tty0): {} — skipping foreground-VT check",
        std::strerror(errno));
    return true;
  }
  struct vt_stat vtstat{};
  const bool got_state = ::ioctl(tty0, VT_GETSTATE, &vtstat) == 0;
  const int vt_errno = errno;
  ::close(tty0);
  if (!got_state) {
    ihs::log::warn(
        "[DrmDisplay] VT_GETSTATE: {} — skipping foreground-VT check",
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
    ihs::log::error(
        "[DrmDisplay] no controlling terminal (open /dev/tty: {}). Cannot "
        "drive {} without a foreground VT — switch to the active console "
        "(tty{}) and rerun.",
        std::strerror(errno), drm_device, active_vt);
    return false;
  }
  // fstat() of the resulting fd does NOT return the underlying tty's
  // device numbers — it returns /dev/tty's own (5, 0) inode metadata on
  // every Linux kernel from 4.x onward. TIOCGDEV is the canonical ioctl
  // for retrieving the actual tty device number from a /dev/tty fd;
  // available since Linux 2.6.18.
  unsigned int dev = 0;
  const int ioctl_rc = ::ioctl(ctl_fd, TIOCGDEV, &dev);
  const int ioctl_errno = errno;
  ::close(ctl_fd);
  if (ioctl_rc != 0) {
    ihs::log::error(
        "[DrmDisplay] TIOCGDEV(/dev/tty): {}. Cannot drive {} without "
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
    ihs::log::error(
        "[DrmDisplay] controlling terminal is not a kernel VT "
        "(major={}, minor={}) — you're running from a terminal emulator or "
        "SSH. Active VT is tty{}. Switch to tty{} (Ctrl+Alt+F{}) and rerun, "
        "or `sudo systemctl isolate multi-user.target` first. Refusing to "
        "drive {}.",
        ctl_major, ctl_minor, active_vt, active_vt, active_vt, drm_device);
    return false;
  }
  if (ctl_minor != active_vt) {
    ihs::log::error(
        "[DrmDisplay] controlling VT is tty{} but foreground VT is tty{} — "
        "scanout goes to the foreground. Switch to tty{} (Ctrl+Alt+F{}) and "
        "rerun. Refusing to drive {}.",
        ctl_minor, active_vt, ctl_minor, ctl_minor, drm_device);
    return false;
  }
  ihs::log::info("[DrmDisplay] foreground VT check: on tty{} (active)",
                 active_vt);
  return true;
}

}  // namespace

DrmDisplay::DrmDisplay(int32_t width,
                       int32_t height,
                       double refresh_rate_hz,
                       std::string device_path,
                       bool no_seat)
    : width_(width),
      height_(height),
      refresh_rate_hz_(refresh_rate_hz),
      device_path_(std::move(device_path)),
      no_seat_(no_seat),
      output_provider_(device_path_),
      session_(OpenSessionOrLog(no_seat)),
      seat_(std::make_unique<homescreen::DrmSeat>(width,
                                                  height,
                                                  OpenerFrom(session_.get()))) {
  // Route Ctrl+Alt+F<n> through libseat. K_OFF (set inside DrmSeat) hides
  // the chord from the kernel keymap layer, so without this hook the
  // user has no way to leave the session short of remoting in and
  // running `chvt`. Only installed when both pieces exist — without a
  // libseat session there's no switch_session API to call.
  //
  // While we're here, also subscribe DrmSeat to session pause/resume
  // so libinput's evdev fds get suspended/resumed across the VT
  // round-trip. DrmBackend installs its own pair separately
  // (compositor pause + ScheduleFrame on resume).
  if (session_) {
    if (auto* drm_seat = dynamic_cast<homescreen::DrmSeat*>(seat_.get())) {
      homescreen::DrmSession* sess = session_.get();
      drm_seat->SetVtSwitchHandler([sess](int vt) -> bool {
        const int err = sess->SwitchSession(vt);
        if (err != 0) {
          ihs::log::warn("[DrmDisplay] SwitchSession({}) failed: {}", vt,
                         std::strerror(err));
          return false;
        }
        return true;
      });
      session_->AddPauseHandler([drm_seat]() { drm_seat->OnSessionPaused(); });
      session_->AddResumeHandler(
          [drm_seat](int /*fd*/) { drm_seat->OnSessionResumed(); });
    }
  }
}

DrmDisplay::~DrmDisplay() {
  // Stop the per-card flip reader first -- it runs its own thread and holds the
  // fd via flip_descriptor_, so it must be torn down before the fd is closed.
  StopFlipReader();
  // Drop master (and let drm_dev_'s destructor close the fd) once the backends
  // that scanned out through it are gone. This display is shared and destroyed
  // after them, so master is released last -- whoever takes over next (fbcon,
  // a getty, the next compositor) inherits a clean device.
  if (drm_master_ && drm_dev_.has_value()) {
    drmDropMaster(drm_dev_->fd());
    drm_master_ = false;
  }
}

drm::Device* DrmDisplay::SharedDevice() {
  if (drm_dev_.has_value()) {
    return &*drm_dev_;
  }

  if (session_ != nullptr) {
    // libseat path: the seat provider (logind/seatd/builtin) owns VT
    // activation and master handoff; TakeDevice returns a master-capable fd
    // while we hold the seat. No drmSetMaster, no foreground-VT guard.
    const int fd = session_->TakeDevice(device_path_);
    if (fd < 0) {
      ihs::log::error("[DrmDisplay] session take_device({}): failed",
                      device_path_);
      return nullptr;
    }
    drm_dev_.emplace(drm::Device::from_fd(fd));
    ihs::log::info("[DrmDisplay] opened {} via libseat (fd={})", device_path_,
                   fd);
  } else {
    // Fallback path: no seat backend (or --drm-no-seat). Refuse up-front if we
    // are not on the active VT, then open + take master directly.
    if (no_seat_) {
      ihs::log::warn(
          "[DrmDisplay] --drm-no-seat: skipping the foreground-VT guard on "
          "{}. If the screen stays black, another DRM master holds the device "
          "or scanout is owned by a different VT.",
          device_path_);
    } else if (!VerifyForegroundVt(device_path_)) {
      return nullptr;
    }

    auto dev = drm::Device::open(device_path_);
    if (!dev) {
      ihs::log::error("[DrmDisplay] open({}): {}", device_path_,
                      dev.error().message());
      return nullptr;
    }
    drm_dev_.emplace(std::move(*dev));

    // Atomic writes from a non-master fd are silently accepted by some drivers
    // (amdgpu) as no-ops, which presents as a blank panel with no
    // PAGE_FLIP_EVENT. Refuse to run without master.
    if (drmSetMaster(drm_dev_->fd()) != 0) {
      if (const int err = errno;
          err == EBUSY || err == EACCES || err == EPERM) {
        ihs::log::error(
            "[DrmDisplay] cannot acquire DRM master on {} ({}). Another "
            "display server (gdm / gnome-shell / sddm / Xorg / a Wayland "
            "compositor) is already holding the device. Stop it or run from a "
            "bare TTY (e.g. `sudo systemctl isolate multi-user.target`).",
            device_path_, std::strerror(err));
      } else {
        ihs::log::error("[DrmDisplay] drmSetMaster({}): {}", device_path_,
                        std::strerror(err));
      }
      drm_dev_.reset();
      return nullptr;
    }
    drm_master_ = true;
    ihs::log::info("[DrmDisplay] DRM master on {}", device_path_);
  }
  return &*drm_dev_;
}

void DrmDisplay::SetFlipHandler(PageFlipHandler handler) {
  flip_handler_ = handler;
}

void DrmDisplay::StartFlipReader() {
  if (flip_reader_running_ || !drm_dev_.has_value() ||
      flip_handler_ == nullptr) {
    return;
  }
  flip_ioc_ = std::make_unique<asio::io_context>(ASIO_CONCURRENCY_HINT_1);
  flip_work_.emplace(asio::make_work_guard(*flip_ioc_));
  flip_descriptor_.emplace(*flip_ioc_);
  // assign() makes asio own the fd; StopFlipReader releases it before reset so
  // drm_dev_ (the fd owner) is not double-closed.
  flip_descriptor_->assign(drm_dev_->fd());
  ArmFlipRead();
  flip_thread_ = std::thread([this]() { flip_ioc_->run(); });
  flip_reader_running_ = true;
  ihs::log::info("[DrmDisplay] flip reader started on fd={}", drm_dev_->fd());
}

void DrmDisplay::ArmFlipRead() {
  if (!flip_descriptor_.has_value()) {
    return;
  }
  flip_descriptor_->async_wait(
      asio::posix::stream_descriptor::wait_read,
      [this](const std::error_code& ec) {
        if (ec) {
          // operation_aborted fires on cancel() at teardown -- quiet path.
          if (ec != asio::error::operation_aborted) {
            ihs::log::warn("[DrmDisplay] flip reader wait: {}", ec.message());
          }
          return;
        }
        DrainFlip();
        ArmFlipRead();  // re-arm for the next vblank
      });
}

void DrmDisplay::DrainFlip() {
  // The reader thread's re-arm callback: drain whatever is ready right now.
  (void)DrainReadyFlips(0);
}

bool DrmDisplay::DrainReadyFlips(const int timeout_ms) {
  if (!drm_dev_.has_value() || flip_handler_ == nullptr) {
    return false;
  }
  const int fd = drm_dev_->fd();
  // Wait for readability WITHOUT the lock so the reader thread and the raster
  // thread can both block here; whichever wins the drain locks below.
  pollfd pfd{fd, POLLIN, 0};
  if (::poll(&pfd, 1, timeout_ms) <= 0 || (pfd.revents & POLLIN) == 0) {
    return false;
  }
  // Serialize the actual fd read: two threads may have seen it readable, but
  // only one may consume the event. The loser re-polls(0) below, finds nothing,
  // and returns -- no double-read, no lost event. The handler (user_data == the
  // committing sink) records the flip, clears its flip_pending_, and returns
  // the vsync baton; safe from either thread (flip_pending_ is atomic and
  // DeliverVsync marshals onto the runner strand).
  const std::lock_guard<std::mutex> lock(flip_drain_mu_);
  pollfd pfd2{fd, POLLIN, 0};
  if (::poll(&pfd2, 1, 0) <= 0 || (pfd2.revents & POLLIN) == 0) {
    return false;
  }
  drmEventContext ctx{};
  ctx.version = 2;
  ctx.page_flip_handler = flip_handler_;
  drmHandleEvent(fd, &ctx);
  return true;
}

void DrmDisplay::StopFlipReader() {
  if (!flip_reader_running_) {
    return;
  }
  if (flip_descriptor_.has_value()) {
    std::error_code ec;
    flip_descriptor_->cancel(ec);       // wake the reader out of epoll_wait
    (void)flip_descriptor_->release();  // detach the fd; drm_dev_ closes it
    flip_descriptor_.reset();
  }
  flip_work_.reset();  // drop the work guard so run() can return
  if (flip_ioc_) {
    flip_ioc_->stop();
  }
  if (flip_thread_.joinable()) {
    flip_thread_.join();
  }
  flip_ioc_.reset();
  flip_reader_running_ = false;
}

void DrmDisplay::ReservePlanes(const std::vector<uint32_t>& planes) {
  const std::lock_guard<std::mutex> lock(reserved_planes_mu_);
  for (const uint32_t id : planes) {
    if (std::find(reserved_planes_.begin(), reserved_planes_.end(), id) ==
        reserved_planes_.end()) {
      reserved_planes_.push_back(id);
    }
  }
}

std::vector<uint32_t> DrmDisplay::ReservedPlanes() const {
  const std::lock_guard<std::mutex> lock(reserved_planes_mu_);
  return reserved_planes_;
}

void DrmDisplay::StartEvents() {
  if (seat_) {
    seat_->Start();
  }
}

void DrmDisplay::StopEvents() {
  if (seat_) {
    seat_->Stop();
  }
}

void DrmDisplay::SetViewControllerState(
    FlutterDesktopViewControllerState* state) {
  view_controller_state_ = state;
  if (seat_) {
    seat_->SetViewControllerState(state);
  }
}

void DrmDisplay::SetViewportSize(const int32_t width, const int32_t height) {
  width_ = width;
  height_ = height;
  if (auto* drm_seat = dynamic_cast<homescreen::DrmSeat*>(seat_.get())) {
    drm_seat->SetViewport(width, height);
  }
}

void DrmDisplay::SetCursor(homescreen::DrmCursor* cursor) {
  // The seat's polymorphic base ISeat has no cursor hook — that's
  // intentional, only the DRM seat drives a KMS cursor. Cast to the
  // concrete type and forward; the cast fails only if the build mixes
  // a non-DRM seat with this display, which doesn't happen today.
  if (auto* drm_seat = dynamic_cast<homescreen::DrmSeat*>(seat_.get())) {
    drm_seat->SetCursor(cursor);
  }
}

void DrmDisplay::SetGlCursor(homescreen::ICursorPositionSink* sink) {
  if (auto* drm_seat = dynamic_cast<homescreen::DrmSeat*>(seat_.get())) {
    drm_seat->SetGlCursor(sink);
  }
}

void DrmDisplay::SetCursorRotation(const int32_t degrees) {
  if (auto* drm_seat = dynamic_cast<homescreen::DrmSeat*>(seat_.get())) {
    drm_seat->SetCursorRotation(degrees);
  }
}

void DrmDisplay::SetCursorShapeSink(homescreen::ICursorShapeSink* sink) {
  shape_sink_ = sink;
}

bool DrmDisplay::ActivateSystemCursor(const int32_t /*device*/,
                                      const std::string& kind) const {
  if (shape_sink_ == nullptr) {
    return true;
  }
  return shape_sink_->SetShape(homescreen::CursorKindToXcursorName(kind));
}

void DrmDisplay::SetInputTransforms(const std::vector<std::string>& specs) {
  if (auto* drm_seat = dynamic_cast<homescreen::DrmSeat*>(seat_.get())) {
    drm_seat->SetInputTransforms(specs);
  }
}

void DrmDisplay::SetRegionLayout(const int32_t x, const int32_t y) {
  if (auto* drm_seat = dynamic_cast<homescreen::DrmSeat*>(seat_.get())) {
    drm_seat->SetRegionLayout(x, y);
  }
}

void DrmDisplay::SetRegionTouchDevice(const std::string& name) {
  if (auto* drm_seat = dynamic_cast<homescreen::DrmSeat*>(seat_.get())) {
    drm_seat->SetRegionTouchDevice(name);
  }
}
