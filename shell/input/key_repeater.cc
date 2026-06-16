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

#include "input/key_repeater.h"

#include <sys/timerfd.h>
#include <unistd.h>

#include <cstdint>
#include <ctime>

#include "input/xkb_keyboard.h"

namespace homescreen::input {

namespace {

timespec MsToTimespec(uint32_t ms) {
  return timespec{static_cast<time_t>(ms / 1000),
                  static_cast<long>((ms % 1000) * 1'000'000L)};
}

}  // namespace

std::unique_ptr<KeyRepeater> KeyRepeater::Create(const XkbKeyboard* keyboard,
                                                 RepeatConfig cfg) {
  if (keyboard == nullptr) {
    return nullptr;
  }
  const int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (fd < 0) {
    return nullptr;
  }
  return std::unique_ptr<KeyRepeater>(new KeyRepeater(keyboard, cfg, fd));
}

KeyRepeater::KeyRepeater(const XkbKeyboard* keyboard,
                         RepeatConfig cfg,
                         int timer_fd)
    : kb_(keyboard), cfg_(cfg), timer_fd_(timer_fd) {}

KeyRepeater::~KeyRepeater() {
  if (timer_fd_ >= 0) {
    ::close(timer_fd_);
  }
}

void KeyRepeater::OnKey(const uint32_t evdev_keycode, const bool pressed) {
  if (pressed) {
    // Only repeatable keys arm; a non-repeating press (Shift/Ctrl/dead key)
    // leaves any in-flight repeat alone. The most recently pressed repeatable
    // key wins (matches wl_keyboard.repeat_info and desktop UX).
    if (kb_->KeyRepeats(evdev_keycode)) {
      held_key_ = evdev_keycode;
      is_held_ = true;
      Arm();
    }
  } else if (is_held_ && evdev_keycode == held_key_) {
    is_held_ = false;
    Disarm();
  }
}

void KeyRepeater::Arm() {
  if (timer_fd_ < 0) {
    return;
  }
  // Fire once after the initial delay, then every interval.
  const itimerspec its{MsToTimespec(cfg_.interval_ms),
                       MsToTimespec(cfg_.delay_ms)};
  ::timerfd_settime(timer_fd_, 0, &its, nullptr);
}

void KeyRepeater::Disarm() {
  if (timer_fd_ < 0) {
    return;
  }
  const itimerspec its{};  // all-zero disarms
  ::timerfd_settime(timer_fd_, 0, &its, nullptr);
}

void KeyRepeater::Dispatch() {
  if (timer_fd_ < 0) {
    return;
  }
  uint64_t expirations = 0;
  // Drain the timerfd; the count is consumed but we emit a single repeat per
  // dispatch (the poll loop runs at least as often as the interval, and
  // emitting a burst on a stalled loop would just spam stale events).
  const ssize_t n = ::read(timer_fd_, &expirations, sizeof(expirations));
  (void)n;
  if (!is_held_ || !handler_) {
    return;
  }
  std::string utf8;
  const xkb_keysym_t sym = kb_->ResolveSym(held_key_, &utf8);
  handler_(held_key_, sym, utf8);
}

void KeyRepeater::Cancel() {
  is_held_ = false;
  Disarm();
}

}  // namespace homescreen::input