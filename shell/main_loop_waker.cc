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

#include "main_loop_waker.h"

#include <atomic>
#include <cerrno>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "logging/logging.h"

namespace {
// Published copy of the eventfd for the async-signal-safe SignalWake() path.
// A plain atomic<int> read/write plus write() to the fd are all
// async-signal-safe; the singleton's lazy init is not, hence this mirror.
std::atomic<int> g_signal_fd{-1};
}  // namespace

MainLoopWaker& MainLoopWaker::instance() {
  static MainLoopWaker waker;
  return waker;
}

MainLoopWaker::MainLoopWaker() : fd_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
  if (fd_ < 0) {
    spdlog::critical("MainLoopWaker: failed to create eventfd: {}",
                     strerror(errno));
  }
  g_signal_fd.store(fd_, std::memory_order_release);
}

MainLoopWaker::~MainLoopWaker() {
  g_signal_fd.store(-1, std::memory_order_release);
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

void MainLoopWaker::Wake() const {
  if (fd_ < 0) {
    return;
  }
  const uint64_t one = 1;
  ssize_t n;
  do {
    n = write(fd_, &one, sizeof(one));
  } while (n < 0 && errno == EINTR);
  // EAGAIN means the 64-bit counter is already saturated, i.e. a wake is
  // pending — nothing more to do.
}

void MainLoopWaker::Wait(const int timeout_ms) const {
  if (fd_ < 0) {
    return;
  }
  pollfd pfd{fd_, POLLIN, 0};
  const int rc = poll(&pfd, 1, timeout_ms);
  if (rc < 0) {
    if (errno != EINTR) {
      spdlog::error("MainLoopWaker: poll failed: {}", strerror(errno));
    }
    return;
  }
  if (rc > 0 && (pfd.revents & POLLIN)) {
    // Drain the counter so the next Wait() blocks again until a fresh Wake().
    uint64_t drained;
    while (read(fd_, &drained, sizeof(drained)) == sizeof(drained)) {
    }
  }
}

void MainLoopWaker::SignalWake() {
  const int fd = g_signal_fd.load(std::memory_order_acquire);
  if (fd < 0) {
    return;
  }
  const uint64_t one = 1;
  // write() is async-signal-safe; ignore the result (best-effort wake).
  const ssize_t n = write(fd, &one, sizeof(one));
  (void)n;
}
