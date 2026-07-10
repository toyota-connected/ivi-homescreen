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

#include "input/wake_event_fd.h"

#include <cerrno>
#include <cstdint>
#include <cstring>

#include <sys/eventfd.h>
#include <unistd.h>

#include "logging/logging.h"

namespace homescreen::input {

WakeEventFd::WakeEventFd() : fd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
  if (fd_ < 0) {
    ihs::log::warn(
        "[WakeEventFd] eventfd() failed ({}); dispatch falls back to timed "
        "poll",
        std::strerror(errno));
  }
}

WakeEventFd::~WakeEventFd() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void WakeEventFd::Wake() const {
  if (fd_ < 0) {
    return;
  }
  const uint64_t one = 1;
  // Single write; the fd becomes (or stays) readable. A saturated counter
  // returns EAGAIN, which is fine — it is already readable. EINTR is retried by
  // the kernel for a write this small in practice; treat any error as "already
  // signaled" since the only failure that matters (fd invalid) can't happen.
  while (::write(fd_, &one, sizeof(one)) < 0 && errno == EINTR) {
  }
}

void WakeEventFd::Drain() const {
  if (fd_ < 0) {
    return;
  }
  uint64_t count = 0;
  // One nonblocking read clears the coalesced counter. EAGAIN means nothing was
  // pending. Loop only on EINTR.
  while (::read(fd_, &count, sizeof(count)) < 0 && errno == EINTR) {
  }
}

}  // namespace homescreen::input
