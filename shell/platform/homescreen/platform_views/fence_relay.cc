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

#include "fence_relay.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <thread>

#include "logging/logging.h"

namespace {

void Signal(const int eventfd) {
  if (eventfd_write(eventfd, 1) != 0) {
    ihs::log::warn(
        "[ihs_pv] release relay: eventfd_write failed (errno={}); the "
        "producer may stall on this slot",
        errno);
  }
  ::close(eventfd);
}

}  // namespace

FenceRelay& FenceRelay::Get() {
  // Never destroyed; see the class comment.
  static auto* const relay = new FenceRelay();
  return *relay;
}

FenceRelay::FenceRelay() : wake_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
  if (wake_ < 0) {
    ihs::log::error("[ihs_pv] release relay: eventfd() failed (errno={})",
                    errno);
    return;
  }
  std::thread([this] { Run(); }).detach();
}

void FenceRelay::Add(const int fence, const int eventfd) {
  if (eventfd < 0) {
    if (fence >= 0) {
      ::close(fence);
    }
    return;
  }
  // No fence, or no thread to wait on one: the wait cannot happen, so do not
  // make the producer wait at all.
  if (fence < 0 || wake_ < 0) {
    if (fence >= 0) {
      ::close(fence);
    }
    Signal(eventfd);
    return;
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    pairs_.push_back({fence, eventfd});
  }
  eventfd_write(wake_, 1);
}

size_t FenceRelay::pending() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return pairs_.size();
}

void FenceRelay::Run() {
  std::vector<pollfd> fds;
  for (;;) {
    fds.clear();
    fds.push_back({wake_, POLLIN, 0});
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      for (const Pair& p : pairs_) {
        fds.push_back({p.fence, POLLIN, 0});
      }
    }
    if (::poll(fds.data(), fds.size(), -1) < 0) {
      if (errno != EINTR) {
        ihs::log::error("[ihs_pv] release relay: poll failed (errno={})",
                        errno);
        return;
      }
      continue;
    }
    if ((fds[0].revents & POLLIN) != 0) {
      eventfd_t drained;
      eventfd_read(wake_, &drained);
    }
    // Pairs are only appended while the thread polls, so the first
    // fds.size() - 1 of them are the ones polled, in order.
    const std::lock_guard<std::mutex> lock(mutex_);
    size_t kept = 0;
    for (size_t i = 0; i < pairs_.size(); ++i) {
      short revents = 0;
      if (i + 1 < fds.size()) {
        revents = fds[i + 1].revents;
      }
      if (revents == 0) {
        pairs_[kept++] = pairs_[i];
        continue;
      }
      if ((revents & POLLIN) == 0) {
        ihs::log::warn(
            "[ihs_pv] release relay: fence error (revents=0x{:x}); releasing "
            "the slot",
            revents);
      }
      ::close(pairs_[i].fence);
      Signal(pairs_[i].eventfd);
    }
    pairs_.resize(kept);
  }
}
