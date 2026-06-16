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

#include "input/xkb_keyboard.h"

#include <vector>

namespace homescreen::input {

namespace {

// evdev keycodes are offset by 8 from xkb's scancode space — the universal
// Linux convention.
constexpr uint32_t kEvdevXkbOffset = 8;

// Build a keymap from RMLVO options. Empty fields fall through to xkbcommon's
// compiled-in defaults and the XKB_DEFAULT_* env vars (passing a names struct
// of all-NULL is equivalent to xkb_keymap_new_from_names(ctx, nullptr, ...)).
xkb_keymap* CompileKeymap(xkb_context* ctx, const KeymapOptions& opts) {
  const xkb_rule_names names{
      opts.rules.empty() ? nullptr : opts.rules.c_str(),
      opts.model.empty() ? nullptr : opts.model.c_str(),
      opts.layout.empty() ? nullptr : opts.layout.c_str(),
      opts.variant.empty() ? nullptr : opts.variant.c_str(),
      opts.options.empty() ? nullptr : opts.options.c_str(),
  };
  const bool any = !opts.rules.empty() || !opts.model.empty() ||
                   !opts.layout.empty() || !opts.variant.empty() ||
                   !opts.options.empty();
  return xkb_keymap_new_from_names(ctx, any ? &names : nullptr,
                                   XKB_KEYMAP_COMPILE_NO_FLAGS);
}

std::string Utf8For(xkb_state* state, uint32_t scancode) {
  const int n = xkb_state_key_get_utf8(state, scancode, nullptr, 0);
  if (n <= 0) {
    return {};
  }
  std::string out(static_cast<size_t>(n), '\0');
  // Buffer must hold the NUL too; n excludes it, so size n+1.
  out.resize(static_cast<size_t>(n) + 1);
  xkb_state_key_get_utf8(state, scancode, out.data(), out.size());
  out.resize(static_cast<size_t>(n));  // drop the trailing NUL
  return out;
}

bool LedActive(xkb_state* state, const char* name) {
  return xkb_state_led_name_is_active(state, name) > 0;
}

}  // namespace

std::unique_ptr<XkbKeyboard> XkbKeyboard::Create(const KeymapOptions& opts) {
  xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (ctx == nullptr) {
    return nullptr;
  }
  xkb_keymap* keymap = CompileKeymap(ctx, opts);
  if (keymap == nullptr) {
    xkb_context_unref(ctx);
    return nullptr;
  }
  xkb_state* state = xkb_state_new(keymap);
  if (state == nullptr) {
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);
    return nullptr;
  }
  return std::unique_ptr<XkbKeyboard>(new XkbKeyboard(ctx, keymap, state));
}

XkbKeyboard::XkbKeyboard(xkb_context* ctx, xkb_keymap* keymap, xkb_state* state)
    : ctx_(ctx), keymap_(keymap), state_(state) {}

XkbKeyboard::~XkbKeyboard() {
  if (state_ != nullptr) {
    xkb_state_unref(state_);
  }
  if (keymap_ != nullptr) {
    xkb_keymap_unref(keymap_);
  }
  if (ctx_ != nullptr) {
    xkb_context_unref(ctx_);
  }
}

xkb_keysym_t XkbKeyboard::ProcessKey(const uint32_t evdev_keycode,
                                     const bool pressed,
                                     std::string* utf8_out) {
  const uint32_t scancode = evdev_keycode + kEvdevXkbOffset;
  const xkb_keysym_t sym = xkb_state_key_get_one_sym(state_, scancode);
  if (utf8_out != nullptr) {
    *utf8_out = Utf8For(state_, scancode);
  }
  xkb_state_update_key(state_, scancode, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
  return sym;
}

xkb_keysym_t XkbKeyboard::ResolveSym(const uint32_t evdev_keycode,
                                     std::string* utf8_out) const {
  const uint32_t scancode = evdev_keycode + kEvdevXkbOffset;
  if (utf8_out != nullptr) {
    *utf8_out = Utf8For(state_, scancode);
  }
  return xkb_state_key_get_one_sym(state_, scancode);
}

bool XkbKeyboard::KeyRepeats(const uint32_t evdev_keycode) const {
  return xkb_keymap_key_repeats(keymap_, evdev_keycode + kEvdevXkbOffset) != 0;
}

uint32_t XkbKeyboard::Modifiers() const {
  return xkb_state_serialize_mods(state_, XKB_STATE_MODS_EFFECTIVE);
}

bool XkbKeyboard::CtrlActive() const {
  return xkb_state_mod_name_is_active(state_, XKB_MOD_NAME_CTRL,
                                      XKB_STATE_MODS_EFFECTIVE) > 0;
}

bool XkbKeyboard::AltActive() const {
  return xkb_state_mod_name_is_active(state_, XKB_MOD_NAME_ALT,
                                      XKB_STATE_MODS_EFFECTIVE) > 0;
}

LedState XkbKeyboard::Leds() const {
  return LedState{
      LedActive(state_, XKB_LED_NAME_CAPS),
      LedActive(state_, XKB_LED_NAME_NUM),
      LedActive(state_, XKB_LED_NAME_SCROLL),
  };
}

bool XkbKeyboard::Reload(const KeymapOptions& opts) {
  xkb_keymap* keymap = CompileKeymap(ctx_, opts);
  if (keymap == nullptr) {
    return false;
  }
  xkb_state* state = xkb_state_new(keymap);
  if (state == nullptr) {
    xkb_keymap_unref(keymap);
    return false;
  }
  xkb_state_unref(state_);
  xkb_keymap_unref(keymap_);
  keymap_ = keymap;
  state_ = state;
  return true;
}

}  // namespace homescreen::input