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

// Mapping from Linux evdev KEY_* codes to Flutter's physical and logical
// key encodings. Flutter's physical keys use a USB-HID-based scheme:
//
//   physical = 0x0007'0000 | usb_hid_usage_id   (keyboard/keypad page 0x07)
//
// Logical keys for printable characters use the Unicode code point of the
// lowercase letter/symbol; non-printable keys use Flutter's own namespace
// (0x0010'0000'0000 range). For practical correctness we derive logical
// keys from the XKB-supplied UTF-8 character when available, and fall
// through to a small non-printable table otherwise.

#pragma once

#include <cstdint>

#include <linux/input-event-codes.h>

namespace homescreen::keys {

// USB HID keyboard/keypad page prefix.
constexpr uint64_t kHidKeyboardPage = 0x00070000ULL;

// Flutter's logical key namespace for non-printable keys.
constexpr uint64_t kFlutterLogicalPlane = 0x0100000000ULL;

// Convert a Linux evdev key code to a Flutter physical key ID.
// Returns 0 for unmapped keys (the engine treats physical=0 as empty).
inline uint64_t EvdevToPhysical(uint32_t evdev) {
  // The table below covers the core 104-key US layout + F-keys + numpad +
  // navigation + modifiers + multimedia. It is compiled from the USB HID
  // Usage Tables (Keyboard/Keypad page 0x07) cross-referenced against
  // linux/input-event-codes.h. Entries are sorted by evdev code.
  //
  // clang-format off
  static constexpr struct { uint32_t evdev; uint16_t hid; } kMap[] = {
    {KEY_ESC,         0x29},
    {KEY_1,           0x1E}, {KEY_2,       0x1F}, {KEY_3,      0x20},
    {KEY_4,           0x21}, {KEY_5,       0x22}, {KEY_6,      0x23},
    {KEY_7,           0x24}, {KEY_8,       0x25}, {KEY_9,      0x26},
    {KEY_0,           0x27},
    {KEY_MINUS,       0x2D}, {KEY_EQUAL,   0x2E},
    {KEY_BACKSPACE,   0x2A}, {KEY_TAB,     0x2B},
    {KEY_Q,           0x14}, {KEY_W,       0x1A}, {KEY_E,      0x08},
    {KEY_R,           0x15}, {KEY_T,       0x17}, {KEY_Y,      0x1C},
    {KEY_U,           0x18}, {KEY_I,       0x0C}, {KEY_O,      0x12},
    {KEY_P,           0x13},
    {KEY_LEFTBRACE,   0x2F}, {KEY_RIGHTBRACE, 0x30},
    {KEY_ENTER,       0x28},
    {KEY_LEFTCTRL,    0xE0},
    {KEY_A,           0x04}, {KEY_S,       0x16}, {KEY_D,      0x07},
    {KEY_F,           0x09}, {KEY_G,       0x0A}, {KEY_H,      0x0B},
    {KEY_J,           0x0D}, {KEY_K,       0x0E}, {KEY_L,      0x0F},
    {KEY_SEMICOLON,   0x33}, {KEY_APOSTROPHE, 0x34},
    {KEY_GRAVE,       0x35},
    {KEY_LEFTSHIFT,   0xE1},
    {KEY_BACKSLASH,   0x31},
    {KEY_Z,           0x1D}, {KEY_X,       0x1B}, {KEY_C,      0x06},
    {KEY_V,           0x19}, {KEY_B,       0x05}, {KEY_N,      0x11},
    {KEY_M,           0x10},
    {KEY_COMMA,       0x36}, {KEY_DOT,     0x37}, {KEY_SLASH,  0x38},
    {KEY_RIGHTSHIFT,  0xE5},
    {KEY_KPASTERISK,  0x55},
    {KEY_LEFTALT,     0xE2},
    {KEY_SPACE,       0x2C},
    {KEY_CAPSLOCK,    0x39},
    {KEY_F1,          0x3A}, {KEY_F2,      0x3B}, {KEY_F3,     0x3C},
    {KEY_F4,          0x3D}, {KEY_F5,      0x3E}, {KEY_F6,     0x3F},
    {KEY_F7,          0x40}, {KEY_F8,      0x41}, {KEY_F9,     0x42},
    {KEY_F10,         0x43},
    {KEY_NUMLOCK,     0x53}, {KEY_SCROLLLOCK, 0x47},
    {KEY_KP7,         0x5F}, {KEY_KP8,     0x60}, {KEY_KP9,    0x61},
    {KEY_KPMINUS,     0x56},
    {KEY_KP4,         0x5C}, {KEY_KP5,     0x5D}, {KEY_KP6,    0x5E},
    {KEY_KPPLUS,      0x57},
    {KEY_KP1,         0x59}, {KEY_KP2,     0x5A}, {KEY_KP3,    0x5B},
    {KEY_KP0,         0x62}, {KEY_KPDOT,   0x63},
    {KEY_F11,         0x44}, {KEY_F12,     0x45},
    {KEY_KPENTER,     0x58},
    {KEY_RIGHTCTRL,   0xE4},
    {KEY_KPSLASH,     0x54},
    {KEY_SYSRQ,       0x46},
    {KEY_RIGHTALT,    0xE6},
    {KEY_HOME,        0x4A}, {KEY_UP,      0x52},
    {KEY_PAGEUP,      0x4B}, {KEY_LEFT,    0x50},
    {KEY_RIGHT,       0x4F}, {KEY_END,     0x4D},
    {KEY_DOWN,        0x51}, {KEY_PAGEDOWN, 0x4E},
    {KEY_INSERT,      0x49}, {KEY_DELETE,  0x4C},
    {KEY_PAUSE,       0x48},
    {KEY_LEFTMETA,    0xE3}, {KEY_RIGHTMETA, 0xE7},
    {KEY_COMPOSE,     0x65},
    // F13–F24
    {KEY_F13,         0x68}, {KEY_F14,     0x69}, {KEY_F15,    0x6A},
    {KEY_F16,         0x6B}, {KEY_F17,     0x6C}, {KEY_F18,    0x6D},
    {KEY_F19,         0x6E}, {KEY_F20,     0x6F}, {KEY_F21,    0x70},
    {KEY_F22,         0x71}, {KEY_F23,     0x72}, {KEY_F24,    0x73},
    // Misc
    {KEY_102ND,       0x64},
    {KEY_KPEQUAL,     0x67},
  };
  // clang-format on

  for (const auto& [ev, hid] : kMap) {
    if (ev == evdev) {
      return kHidKeyboardPage | hid;
    }
  }
  return 0;
}

// Decode the first UTF-8 code point from a null-terminated string.
// Returns 0 on empty/invalid input.
inline uint32_t Utf8ToCodePoint(const char* s) {
  if (!s || !s[0]) {
    return 0;
  }
  const auto c0 = static_cast<uint8_t>(s[0]);
  if (c0 < 0x80) {
    return c0;
  }
  if ((c0 & 0xE0) == 0xC0 && s[1]) {
    return (static_cast<uint32_t>(c0 & 0x1F) << 6) |
           (static_cast<uint32_t>(s[1]) & 0x3F);
  }
  if ((c0 & 0xF0) == 0xE0 && s[1] && s[2]) {
    return (static_cast<uint32_t>(c0 & 0x0F) << 12) |
           ((static_cast<uint32_t>(s[1]) & 0x3F) << 6) |
           (static_cast<uint32_t>(s[2]) & 0x3F);
  }
  if ((c0 & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
    return (static_cast<uint32_t>(c0 & 0x07) << 18) |
           ((static_cast<uint32_t>(s[1]) & 0x3F) << 12) |
           ((static_cast<uint32_t>(s[2]) & 0x3F) << 6) |
           (static_cast<uint32_t>(s[3]) & 0x3F);
  }
  return 0;
}

// Derive the Flutter logical key from the keyboard event. Printable
// characters use their Unicode code point (lowercased for Latin letters
// so the logical key is shift-invariant). Non-printable keys fall through
// to the XKB keysym, which the Flutter framework can still match against
// `LogicalKeyboardKey` constants (they're derived from the same Unicode /
// XKB namespace in the framework's key_data generator).
inline uint64_t DeriveLogicalKey(const char* utf8, uint32_t xkb_sym) {
  const uint32_t cp = Utf8ToCodePoint(utf8);
  if (cp >= 0x20 && cp != 0x7F) {
    // Printable. Lowercase Latin letters so Shift+A and A share the
    // same logical key (Flutter convention).
    if (cp >= 'A' && cp <= 'Z') {
      return cp + ('a' - 'A');
    }
    return cp;
  }
  // Non-printable: pass the XKB keysym. Flutter's LogicalKeyboardKey
  // constants for non-printable keys are numerically identical to the
  // XKB keysyms in the 0xFF00–0xFFFF range (BackSpace, Tab, Return,
  // Escape, Delete, arrows, Home/End/PgUp/PgDn, F1–F24, modifiers).
  // The framework resolves them at the Dart layer.
  return xkb_sym;
}

}  // namespace homescreen::keys
