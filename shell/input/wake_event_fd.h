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

#pragma once

namespace homescreen::input {

// RAII eventfd used to break a libinput-dispatch thread out of an infinite
// poll() when a cross-thread flag (stop, session pause/resume, keymap reload)
// is set. One instance per seat.
//
// Prior art is MainLoopWaker, but that is a process-wide singleton for the main
// loop; each seat needs its own channel, so this is a small owned value type
// rather than a shared singleton.
//
// Counter semantics: Wake() is a single write() — async-signal-safe, lock-free,
// callable from any thread (including the libseat dispatch thread). Consumers
// level-drain in Drain(), so N wakes between polls coalesce into one poll
// return. The lost-wake race (flag set after the consumer's check, before it
// blocks) cannot occur: the producer stores the flag then writes the fd, the fd
// stays readable across the check/block boundary, and the consumer re-reads the
// flag at the top of its next iteration.
class WakeEventFd {
 public:
  // eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK). On failure fd_ is -1 and a warning
  // is logged once; the seat then falls back to a timed poll (degraded mode).
  WakeEventFd();
  ~WakeEventFd();

  WakeEventFd(const WakeEventFd&) = delete;
  WakeEventFd& operator=(const WakeEventFd&) = delete;
  WakeEventFd(WakeEventFd&&) = delete;
  WakeEventFd& operator=(WakeEventFd&&) = delete;

  // The backing eventfd for the seat's poll set. -1 in degraded mode.
  [[nodiscard]] int fd() const { return fd_; }

  // Make fd() readable. Safe from any thread; a no-op in degraded mode. A full
  // counter (EAGAIN) is ignored — the fd is already readable, which is all we
  // need.
  void Wake() const;

  // Clear the readable state. One nonblocking read drains any coalesced count.
  // Safe to call when nothing is pending (EAGAIN) and in degraded mode.
  void Drain() const;

 private:
  int fd_;
};

}  // namespace homescreen::input
