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

#include <poll.h>
#include <cerrno>
#include <csignal>
#include <cstring>
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
  seat_.set_pause_callback([]() {
    spdlog::warn(
        "[DrmSession] session preempted (VT switch-out) — raising SIGTERM");
    // Signal the process-wide shutdown handler installed in main. raise()
    // delivers to this thread; SIGTERM's handler is async-signal-safe and
    // just flips the global `running` flag, which the main loop observes
    // on its next iteration.
    raise(SIGTERM);
  });
  seat_.set_resume_callback([](std::string_view path, int new_fd) {
    spdlog::warn(
        "[DrmSession] resume callback fired for {} (new_fd={}) — "
        "pause/resume handling is stubbed; the process should already "
        "be shutting down",
        std::string(path), new_fd);
  });

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
  auto handle = seat_.take_device(path);
  if (!handle) {
    return -1;
  }
  return handle->fd;
}

drm::input::InputDeviceOpener DrmSession::InputOpener() {
  return seat_.input_opener();
}

void DrmSession::DispatchLoop() {
  const int fd = seat_.poll_fd();
  if (fd < 0) {
    // No backend-backed poll fd — shouldn't happen because Open() would
    // have returned nullptr, but fail soft rather than busy-looping.
    spdlog::warn("[DrmSession] poll_fd() returned -1; dispatcher exiting");
    return;
  }
  while (!stop_.load(std::memory_order_acquire)) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int r = poll(&pfd, 1, kPollTimeoutMs);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      spdlog::error("[DrmSession] poll: {}", std::strerror(errno));
      break;
    }
    if (r > 0 && (pfd.revents & POLLIN) != 0) {
      seat_.dispatch();
    }
  }
}

}  // namespace homescreen
