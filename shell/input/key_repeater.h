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

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <xkbcommon/xkbcommon.h>

namespace homescreen::input {

class XkbKeyboard;

// Auto-repeat timing. Defaults match X11/Wayland convention (600 ms initial
// delay, 25 Hz thereafter) — and, deliberately, drm-cxx's KeyRepeater, so every
// backend repeats identically.
struct RepeatConfig {
  uint32_t delay_ms{600};
  uint32_t interval_ms{40};  // 25 Hz (1000 / 40)
};

// timerfd-driven key auto-repeat shared by every libinput-backed seat. The
// kernel does not synthesize repeats for evdev/libinput, so each seat: feeds
// every real key into OnKey(), polls fd() alongside its libinput fd, and calls
// Dispatch() when fd() is readable. Synthesized repeats are delivered through
// the handler with the keysym/UTF-8 RE-RESOLVED against the keyboard's current
// state each tick (so Shift/AltGr level changes mid-hold take effect on the
// next repeat). Per-key eligibility comes from XkbKeyboard::KeyRepeats(), so
// modifiers / lock / dead keys never repeat. Single-threaded: all calls from
// the owning input-dispatch thread.
class KeyRepeater {
 public:
  // handler(evdev_keycode, keysym, utf8) — called once per repeat tick.
  using Handler =
      std::function<void(uint32_t, xkb_keysym_t, const std::string&)>;

  // @p keyboard must outlive the repeater (the seat owns both). Returns nullptr
  // if the timerfd can't be created (the seat then runs with no auto-repeat).
  static std::unique_ptr<KeyRepeater> Create(const XkbKeyboard* keyboard,
                                             RepeatConfig cfg = {});

  ~KeyRepeater();
  KeyRepeater(const KeyRepeater&) = delete;
  KeyRepeater& operator=(const KeyRepeater&) = delete;

  void SetHandler(Handler handler) { handler_ = std::move(handler); }

  // Feed a real key event. Pressing a repeatable key (re)arms the repeat with
  // that key as the held key (most-recent wins). Releasing the held key
  // disarms. A non-repeating press leaves an existing repeat undisturbed.
  void OnKey(uint32_t evdev_keycode, bool pressed);

  // timerfd for poll/epoll integration. -1 if unavailable (no repeat).
  [[nodiscard]] int fd() const noexcept { return timer_fd_; }

  // Drain the timerfd and emit one repeat for the held key (if any).
  void Dispatch();

  // Drop any in-flight repeat — e.g. on session pause / focus loss.
  void Cancel();

  [[nodiscard]] uint32_t held_key() const noexcept {
    return is_held_ ? held_key_ : 0;
  }

 private:
  KeyRepeater(const XkbKeyboard* keyboard, RepeatConfig cfg, int timer_fd);
  void Arm();
  void Disarm();

  const XkbKeyboard* kb_;
  RepeatConfig cfg_;
  Handler handler_;
  int timer_fd_{-1};
  uint32_t held_key_{0};
  bool is_held_{false};
};

}  // namespace homescreen::input