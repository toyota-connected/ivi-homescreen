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

#include "backend/drm_kms_egl/drm_session.h"
#include "logging/logging.h"

#include <poll.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

#include "logging.h"

namespace homescreen {

namespace {
// Short enough that a Stop() during VT activity feels responsive; long
// enough that the thread doesn't wake needlessly on an idle system.
constexpr int kPollTimeoutMs = 200;
}  // namespace

std::unique_ptr<DrmSession> DrmSession::Open() {
  auto seat_opt = drm::session::Seat::open();
  if (!seat_opt) {
    return nullptr;
  }
  // make_unique can't see the private ctor; use new directly.
  return std::unique_ptr<DrmSession>(new DrmSession(std::move(*seat_opt)));
}

DrmSession::DrmSession(drm::session::Seat seat) : seat_(std::move(seat)) {
  // Set callbacks before starting the dispatch thread so they're live
  // for any event the first dispatch() call drains.
  //
  // Default pause behavior (no lifecycle handler installed yet): raise
  // SIGTERM so a partially-initialized process exits cleanly rather
  // than wedging the seat. Once DrmBackend calls SetLifecycleHandlers,
  // the installed handler takes over and the process stays alive
  // across VT switches.
  seat_.set_pause_callback([this]() {
    std::vector<PauseFn> handlers;
    {
      std::lock_guard lk(lifecycle_mu_);
      handlers = pause_handlers_;
    }
    if (!handlers.empty()) {
      for (auto& h : handlers) {
        h();
      }
      return;
    }
    ihs::log::warn(
        "[DrmSession] session preempted (VT switch-out) before lifecycle "
        "handlers installed — raising SIGTERM");
    // raise() delivers to this thread; SIGTERM's handler is async-
    // signal-safe and just flips the global `running` flag, which the
    // main loop observes on its next iteration.
    raise(SIGTERM);
  });
  seat_.set_resume_callback([this](std::string_view path, int new_fd) {
    std::vector<ResumeFn> handlers;
    {
      std::lock_guard lk(lifecycle_mu_);
      handlers = resume_handlers_;
    }
    if (!handlers.empty()) {
      for (auto& h : handlers) {
        h(new_fd);
      }
      return;
    }
    ihs::log::warn(
        "[DrmSession] resume for {} (fd={}) ignored — no lifecycle handler",
        std::string(path), new_fd);
  });

  // Open the DRM hotplug monitor unless explicitly gated off. Failure
  // is non-fatal — the rest of the session works without hotplug, just
  // without connector-change notifications.
  const char* hotplug_gate = std::getenv("IVI_DRM_HOTPLUG");
  if (hotplug_gate == nullptr || std::string_view(hotplug_gate) != "0") {
    if (auto hp = drm::display::HotplugMonitor::open()) {
      hotplug_.emplace(std::move(*hp));
      hotplug_->set_handler([this](const drm::display::HotplugEvent& e) {
        HotplugHandler local;
        {
          std::lock_guard lk(hotplug_handler_mu_);
          local = hotplug_handler_;
        }
        if (local) {
          local(e);
        }
      });
    } else {
      ihs::log::warn("[DrmSession] hotplug monitor unavailable: {}",
                     hp.error().message());
    }
  }

  thread_ = std::thread(&DrmSession::DispatchLoop, this);
}

DrmSession::~DrmSession() {
  stop_.store(true, std::memory_order_release);
  if (thread_.joinable()) {
    thread_.join();
  }
  // drm::session::Seat destructor closes every tracked device fd and
  // releases the seat itself.
}

int DrmSession::TakeDevice(const std::string& path) {
  // preserve_fd_across_resume = true: logind/seatd revoke the master
  // capability without invalidating the fd integer. Keeping the same
  // fd lets DrmBackend keep its GBM / EGL / property-blob / FB-ID
  // state alive across VT switches and just re-modeset on resume.
  drm::session::TakeDeviceOpts opts{};
  opts.preserve_fd_across_resume = true;
  auto handle = seat_.take_device(path, opts);
  if (!handle) {
    return -1;
  }
  return handle->fd;
}

drm::input::InputDeviceOpener DrmSession::InputOpener() {
  return seat_.input_opener();
}

int DrmSession::SwitchSession(const int session_num) {
  auto r = seat_.switch_session(session_num);
  if (!r) {
    return r.error().value();
  }
  return 0;
}

void DrmSession::AddPauseHandler(PauseFn on_pause) {
  std::lock_guard lk(lifecycle_mu_);
  pause_handlers_.push_back(std::move(on_pause));
}

void DrmSession::AddResumeHandler(ResumeFn on_resume) {
  std::lock_guard lk(lifecycle_mu_);
  resume_handlers_.push_back(std::move(on_resume));
}

void DrmSession::DispatchLoop() {
  const int seat_fd = seat_.poll_fd();
  if (seat_fd < 0) {
    // No backend-backed poll fd — shouldn't happen because Open() would
    // have returned nullptr, but fail soft rather than busy-looping.
    ihs::log::warn("[DrmSession] poll_fd() returned -1; dispatcher exiting");
    return;
  }
  int hp_fd = hotplug_ ? hotplug_->fd() : -1;
  constexpr short kErr = POLLERR | POLLHUP | POLLNVAL;

  while (!stop_.load(std::memory_order_acquire)) {
    pollfd pfds[2];
    pfds[0] = {seat_fd, POLLIN, 0};
    nfds_t nfds = 1;
    if (hp_fd >= 0) {
      pfds[1] = {hp_fd, POLLIN, 0};
      nfds = 2;
    }
    const int r = poll(pfds, nfds, kPollTimeoutMs);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      ihs::log::error("[DrmSession] poll: {}", std::strerror(errno));
      break;
    }
    if (r > 0) {
      if ((pfds[0].revents & kErr) != 0) {
        ihs::log::error("[DrmSession] seat fd error (revents={:#x}); exiting",
                        static_cast<unsigned>(pfds[0].revents));
        break;
      }
      if ((pfds[0].revents & POLLIN) != 0) {
        seat_.dispatch();
      }
      if (nfds == 2) {
        if ((pfds[1].revents & kErr) != 0) {
          // Drop the hotplug monitor and keep serving the seat. A bad
          // hotplug fd shouldn't silently busy-loop the seat dispatcher.
          ihs::log::warn(
              "[DrmSession] hotplug fd error (revents={:#x}); disabling "
              "monitor",
              static_cast<unsigned>(pfds[1].revents));
          hotplug_.reset();
          hp_fd = -1;
        } else if ((pfds[1].revents & POLLIN) != 0) {
          if (auto rc = hotplug_->dispatch(); !rc) {
            ihs::log::warn("[DrmSession] hotplug dispatch: {}",
                           rc.error().message());
          }
        }
      }
    }
  }
}

void DrmSession::set_hotplug_handler(HotplugHandler handler) {
  std::lock_guard lk(hotplug_handler_mu_);
  hotplug_handler_ = std::move(handler);
}

}  // namespace homescreen
