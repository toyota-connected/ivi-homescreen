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
#include <memory>
#include <string>

#include <xkbcommon/xkbcommon.h>

namespace homescreen::input {

// RMLVO keymap selector. All-empty uses xkbcommon's compiled-in defaults plus
// the XKB_DEFAULT_* environment variables (the common case).
struct KeymapOptions {
  std::string rules;
  std::string model;
  std::string layout;
  std::string variant;
  std::string options;
};

// Caps / Num / Scroll Lock latch snapshot, as tracked by xkb.
struct LedState {
  bool caps_lock{false};
  bool num_lock{false};
  bool scroll_lock{false};
};

// Owns an xkbcommon context + keymap + state and translates raw evdev keycodes
// into keysyms / UTF-8 / modifier masks. Shared by every libinput-backed seat
// (DrmSeat, SoftwareSeat) so keyboard translation is identical across backends
// and depends only on libxkbcommon — no drm-cxx.
//
// Callers pass RAW libinput keycodes (the evdev value); the universal evdev→xkb
// +8 offset is applied internally. Single-threaded: all calls must come from
// the owning input-dispatch thread.
class XkbKeyboard {
 public:
  // Build from RMLVO options (empty = xkb defaults / env). Returns nullptr if
  // the context, keymap, or state can't be created.
  static std::unique_ptr<XkbKeyboard> Create(const KeymapOptions& opts = {});

  ~XkbKeyboard();
  XkbKeyboard(const XkbKeyboard&) = delete;
  XkbKeyboard& operator=(const XkbKeyboard&) = delete;

  // Resolve the keysym for @p evdev_keycode at the current state AND advance
  // the modifier state for the press/release. Fills @p utf8_out (if non-null)
  // with the key's UTF-8 string. Call once per real key event.
  xkb_keysym_t ProcessKey(uint32_t evdev_keycode,
                          bool pressed,
                          std::string* utf8_out = nullptr);

  // Resolve the keysym/UTF-8 for @p evdev_keycode at the current state WITHOUT
  // mutating it — used by KeyRepeater to re-resolve a held key each tick so a
  // Shift/AltGr level change mid-hold takes effect on the next repeat.
  [[nodiscard]] xkb_keysym_t ResolveSym(uint32_t evdev_keycode,
                                        std::string* utf8_out = nullptr) const;

  // True when the keymap marks @p evdev_keycode as auto-repeating (modifiers /
  // lock / dead keys are not).
  [[nodiscard]] bool KeyRepeats(uint32_t evdev_keycode) const;

  // Current effective modifier mask (depressed | latched | locked), in
  // xkbcommon's modifier-index bitspace — handed to the embedder.
  [[nodiscard]] uint32_t Modifiers() const;

  // Whether a Ctrl / Alt modifier is currently effective — for chord detection
  // (e.g. the VT-switch Ctrl+Alt+Fn) without decoding the raw mask.
  [[nodiscard]] bool CtrlActive() const;
  [[nodiscard]] bool AltActive() const;

  // Caps/Num/Scroll Lock latch, for drivers that surface keyboard LEDs.
  [[nodiscard]] LedState Leds() const;

  // Swap in a new keymap (e.g. layout change). Preserves nothing of the old
  // latch state. Returns false on failure, leaving the previous keymap intact.
  bool Reload(const KeymapOptions& opts);

 private:
  XkbKeyboard(xkb_context* ctx, xkb_keymap* keymap, xkb_state* state);

  xkb_context* ctx_{nullptr};
  xkb_keymap* keymap_{nullptr};
  xkb_state* state_{nullptr};
};

}  // namespace homescreen::input