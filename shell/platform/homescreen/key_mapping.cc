// Copyright 2020 Toyota Connected North America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "key_mapping.h"

#include <cstdint>
#include <unordered_map>

namespace key_mapping {

// ---------------------------------------------------------------------------
// evdev keycode → USB HID usage (page 0x07 / Keyboard/Keypad page).
// Only keys that require an explicit mapping are listed; all others fall
// through to the Linux-plane fallback (0x00600000 | evdev_code).
//
// evdev codes come from <linux/input-event-codes.h>; USB HID usages from
// https://usb.org/sites/default/files/hut1_3_0.pdf §10.
// ---------------------------------------------------------------------------

// clang-format off
static const std::unordered_map<uint32_t, uint32_t> kEvdevToHid = {
  // Alpha keys — evdev codes from <linux/input-event-codes.h>,
  // HID usages from USB HID usage table (Keyboard/Keypad page 0x07).
  // Note: Linux evdev alpha codes do NOT follow alphabetical order.
  {16, 0x14},  // KEY_Q → HID q
  {17, 0x1a},  // KEY_W → HID w
  {18, 0x08},  // KEY_E → HID e
  {19, 0x15},  // KEY_R → HID r
  {20, 0x17},  // KEY_T → HID t
  {21, 0x1c},  // KEY_Y → HID y
  {22, 0x18},  // KEY_U → HID u
  {23, 0x0c},  // KEY_I → HID i
  {24, 0x12},  // KEY_O → HID o
  {25, 0x13},  // KEY_P → HID p
  {30, 0x04},  // KEY_A → HID a
  {31, 0x16},  // KEY_S → HID s
  {32, 0x07},  // KEY_D → HID d
  {33, 0x09},  // KEY_F → HID f
  {34, 0x0a},  // KEY_G → HID g
  {35, 0x0b},  // KEY_H → HID h
  {36, 0x0d},  // KEY_J → HID j
  {37, 0x0e},  // KEY_K → HID k
  {38, 0x0f},  // KEY_L → HID l
  {44, 0x1d},  // KEY_Z → HID z
  {45, 0x1b},  // KEY_X → HID x
  {46, 0x06},  // KEY_C → HID c
  {47, 0x19},  // KEY_V → HID v
  {48, 0x05},  // KEY_B → HID b
  {49, 0x11},  // KEY_N → HID n
  {50, 0x10},  // KEY_M → HID m

  // Digits — evdev 2..11 (KEY_1..KEY_0)
  { 2, 0x1e},  // KEY_1
  { 3, 0x1f},  // KEY_2
  { 4, 0x20},  // KEY_3
  { 5, 0x21},  // KEY_4
  { 6, 0x22},  // KEY_5
  { 7, 0x23},  // KEY_6
  { 8, 0x24},  // KEY_7
  { 9, 0x25},  // KEY_8
  {10, 0x26},  // KEY_9
  {11, 0x27},  // KEY_0

  // Control keys
  { 1, 0x29},  // KEY_ESC       → Escape
  {14, 0x2a},  // KEY_BACKSPACE → Backspace
  {15, 0x2b},  // KEY_TAB       → Tab
  {28, 0x28},  // KEY_ENTER     → Return
  {57, 0x2c},  // KEY_SPACE     → Space
  {58, 0x39},  // KEY_CAPSLOCK  → Caps Lock

  // Punctuation / symbols
  {12, 0x2d},  // KEY_MINUS
  {13, 0x2e},  // KEY_EQUAL
  {26, 0x2f},  // KEY_LEFTBRACE
  {27, 0x30},  // KEY_RIGHTBRACE
  {43, 0x31},  // KEY_BACKSLASH
  {39, 0x33},  // KEY_SEMICOLON
  {40, 0x34},  // KEY_APOSTROPHE
  {41, 0x35},  // KEY_GRAVE
  {51, 0x36},  // KEY_COMMA
  {52, 0x37},  // KEY_DOT
  {53, 0x38},  // KEY_SLASH

  // F-keys — evdev 59..88 (KEY_F1..KEY_F12, KEY_F13..KEY_F24)
  {59, 0x3a},  // KEY_F1
  {60, 0x3b},  // KEY_F2
  {61, 0x3c},  // KEY_F3
  {62, 0x3d},  // KEY_F4
  {63, 0x3e},  // KEY_F5
  {64, 0x3f},  // KEY_F6
  {65, 0x40},  // KEY_F7
  {66, 0x41},  // KEY_F8
  {67, 0x42},  // KEY_F9
  {68, 0x43},  // KEY_F10
  {87, 0x44},  // KEY_F11
  {88, 0x45},  // KEY_F12
  {183, 0x68},  // KEY_F13
  {184, 0x69},  // KEY_F14
  {185, 0x6a},  // KEY_F15
  {186, 0x6b},  // KEY_F16
  {187, 0x6c},  // KEY_F17
  {188, 0x6d},  // KEY_F18
  {189, 0x6e},  // KEY_F19
  {190, 0x6f},  // KEY_F20
  {191, 0x70},  // KEY_F21
  {192, 0x71},  // KEY_F22
  {193, 0x72},  // KEY_F23
  {194, 0x73},  // KEY_F24

  // Navigation cluster
  {102, 0x4a},  // KEY_HOME
  {103, 0x52},  // KEY_UP
  {104, 0x4b},  // KEY_PAGEUP
  {105, 0x50},  // KEY_LEFT
  {106, 0x4f},  // KEY_RIGHT
  {107, 0x4d},  // KEY_END
  {108, 0x51},  // KEY_DOWN
  {109, 0x4e},  // KEY_PAGEDOWN
  {110, 0x49},  // KEY_INSERT
  {111, 0x4c},  // KEY_DELETE

  // Numpad
  {69,  0x53},  // KEY_NUMLOCK
  {55,  0x55},  // KEY_KPASTERISK
  {71,  0x5f},  // KEY_KP7
  {72,  0x60},  // KEY_KP8
  {73,  0x61},  // KEY_KP9
  {74,  0x56},  // KEY_KPMINUS
  {75,  0x5c},  // KEY_KP4
  {76,  0x5d},  // KEY_KP5
  {77,  0x5e},  // KEY_KP6
  {78,  0x57},  // KEY_KPPLUS
  {79,  0x59},  // KEY_KP1
  {80,  0x5a},  // KEY_KP2
  {81,  0x5b},  // KEY_KP3
  {82,  0x62},  // KEY_KP0
  {83,  0x63},  // KEY_KPDOT
  {96,  0x58},  // KEY_KPENTER
  {98,  0x54},  // KEY_KPSLASH

  // Modifier keys
  {42,  0xe1},  // KEY_LEFTSHIFT
  {54,  0xe5},  // KEY_RIGHTSHIFT
  {29,  0xe0},  // KEY_LEFTCTRL
  {97,  0xe4},  // KEY_RIGHTCTRL
  {56,  0xe2},  // KEY_LEFTALT
  {100, 0xe6},  // KEY_RIGHTALT
  {125, 0xe3},  // KEY_LEFTMETA
  {126, 0xe7},  // KEY_RIGHTMETA

  // Misc
  {70,  0x47},  // KEY_SCROLLLOCK
  {99,  0x46},  // KEY_SYSRQ / PrintScreen
  {119, 0x48},  // KEY_PAUSE
  {127, 0x65},  // KEY_COMPOSE / Menu
};
// clang-format on

// ---------------------------------------------------------------------------
// USB HID usage → Flutter logical key.
// Logical keys for non-printable keys live in the Flutter HID plane:
//   0x00100070000 + hid_usage  (i.e. 0x100000000 | 0x00700000 | hid_usage)
// Source: flutter/packages/flutter/lib/src/services/keyboard_key.g.dart
// ---------------------------------------------------------------------------

// clang-format off
static const std::unordered_map<uint32_t, uint64_t> kHidToLogical = {
  {0x28, 0x100000000 | 0x0700'0028ULL},  // Enter
  {0x29, 0x100000000 | 0x0700'0029ULL},  // Escape
  {0x2a, 0x100000000 | 0x0700'002aULL},  // Backspace
  {0x2b, 0x100000000 | 0x0700'002bULL},  // Tab
  {0x2c, 0x100000000 | 0x0700'002cULL},  // Space
  {0x39, 0x100000000 | 0x0700'0039ULL},  // CapsLock
  {0x3a, 0x100000000 | 0x0700'003aULL},  // F1
  {0x3b, 0x100000000 | 0x0700'003bULL},  // F2
  {0x3c, 0x100000000 | 0x0700'003cULL},  // F3
  {0x3d, 0x100000000 | 0x0700'003dULL},  // F4
  {0x3e, 0x100000000 | 0x0700'003eULL},  // F5
  {0x3f, 0x100000000 | 0x0700'003fULL},  // F6
  {0x40, 0x100000000 | 0x0700'0040ULL},  // F7
  {0x41, 0x100000000 | 0x0700'0041ULL},  // F8
  {0x42, 0x100000000 | 0x0700'0042ULL},  // F9
  {0x43, 0x100000000 | 0x0700'0043ULL},  // F10
  {0x44, 0x100000000 | 0x0700'0044ULL},  // F11
  {0x45, 0x100000000 | 0x0700'0045ULL},  // F12
  {0x46, 0x100000000 | 0x0700'0046ULL},  // PrintScreen
  {0x47, 0x100000000 | 0x0700'0047ULL},  // ScrollLock
  {0x48, 0x100000000 | 0x0700'0048ULL},  // Pause
  {0x49, 0x100000000 | 0x0700'0049ULL},  // Insert
  {0x4a, 0x100000000 | 0x0700'004aULL},  // Home
  {0x4b, 0x100000000 | 0x0700'004bULL},  // PageUp
  {0x4c, 0x100000000 | 0x0700'004cULL},  // Delete
  {0x4d, 0x100000000 | 0x0700'004dULL},  // End
  {0x4e, 0x100000000 | 0x0700'004eULL},  // PageDown
  {0x4f, 0x100000000 | 0x0700'004fULL},  // ArrowRight
  {0x50, 0x100000000 | 0x0700'0050ULL},  // ArrowLeft
  {0x51, 0x100000000 | 0x0700'0051ULL},  // ArrowDown
  {0x52, 0x100000000 | 0x0700'0052ULL},  // ArrowUp
  {0x53, 0x100000000 | 0x0700'0053ULL},  // NumLock
  {0x54, 0x100000000 | 0x0700'0054ULL},  // NumpadDivide
  {0x55, 0x100000000 | 0x0700'0055ULL},  // NumpadMultiply
  {0x56, 0x100000000 | 0x0700'0056ULL},  // NumpadSubtract
  {0x57, 0x100000000 | 0x0700'0057ULL},  // NumpadAdd
  {0x58, 0x100000000 | 0x0700'0058ULL},  // NumpadEnter
  {0x59, 0x100000000 | 0x0700'0059ULL},  // Numpad1
  {0x5a, 0x100000000 | 0x0700'005aULL},  // Numpad2
  {0x5b, 0x100000000 | 0x0700'005bULL},  // Numpad3
  {0x5c, 0x100000000 | 0x0700'005cULL},  // Numpad4
  {0x5d, 0x100000000 | 0x0700'005dULL},  // Numpad5
  {0x5e, 0x100000000 | 0x0700'005eULL},  // Numpad6
  {0x5f, 0x100000000 | 0x0700'005fULL},  // Numpad7
  {0x60, 0x100000000 | 0x0700'0060ULL},  // Numpad8
  {0x61, 0x100000000 | 0x0700'0061ULL},  // Numpad9
  {0x62, 0x100000000 | 0x0700'0062ULL},  // Numpad0
  {0x63, 0x100000000 | 0x0700'0063ULL},  // NumpadDecimal
  {0x65, 0x100000000 | 0x0700'0065ULL},  // ContextMenu
  {0x68, 0x100000000 | 0x0700'0068ULL},  // F13
  {0x69, 0x100000000 | 0x0700'0069ULL},  // F14
  {0x6a, 0x100000000 | 0x0700'006aULL},  // F15
  {0x6b, 0x100000000 | 0x0700'006bULL},  // F16
  {0x6c, 0x100000000 | 0x0700'006cULL},  // F17
  {0x6d, 0x100000000 | 0x0700'006dULL},  // F18
  {0x6e, 0x100000000 | 0x0700'006eULL},  // F19
  {0x6f, 0x100000000 | 0x0700'006fULL},  // F20
  {0x70, 0x100000000 | 0x0700'0070ULL},  // F21
  {0x71, 0x100000000 | 0x0700'0071ULL},  // F22
  {0x72, 0x100000000 | 0x0700'0072ULL},  // F23
  {0x73, 0x100000000 | 0x0700'0073ULL},  // F24
  {0xe0, 0x100000000 | 0x0700'00e0ULL},  // ControlLeft
  {0xe1, 0x100000000 | 0x0700'00e1ULL},  // ShiftLeft
  {0xe2, 0x100000000 | 0x0700'00e2ULL},  // AltLeft
  {0xe3, 0x100000000 | 0x0700'00e3ULL},  // MetaLeft
  {0xe4, 0x100000000 | 0x0700'00e4ULL},  // ControlRight
  {0xe5, 0x100000000 | 0x0700'00e5ULL},  // ShiftRight
  {0xe6, 0x100000000 | 0x0700'00e6ULL},  // AltRight
  {0xe7, 0x100000000 | 0x0700'00e7ULL},  // MetaRight
};
// clang-format on

// ---------------------------------------------------------------------------

uint64_t XkbScancodeToPhysicalKey(const uint32_t xkb_scancode) {
  // xkb_scancode = evdev + 8; recover evdev.
  if (xkb_scancode < 8) {
    return 0;
  }
  const uint32_t evdev = xkb_scancode - 8;

  const auto it = kEvdevToHid.find(evdev);
  if (it != kEvdevToHid.end()) {
    return UINT64_C(0x00700000) | it->second;
  }
  // Fallback: Linux code plane.
  return UINT64_C(0x00600000) | evdev;
}

uint64_t KeysymToLogicalKey(const xkb_keysym_t keysym, const uint32_t utf32) {
  // Printable Unicode characters: logical key == Unicode code point.
  if (utf32 >= 0x20 && utf32 != 0x7f) {
    return static_cast<uint64_t>(utf32);
  }

  // For non-printable keys look up via HID (we must convert keysym → HID
  // via the same evdev→HID table indirectly — easier to do a direct keysym
  // lookup via a small well-known set first, then fall back).
  //
  // Most of the non-printable keysyms we care about are covered by the
  // kHidToLogical table combined with XkbScancodeToPhysicalKey. However,
  // KeysymToLogicalKey is called independently of scancode context (e.g. on
  // key-up when we have the logical key already stored). Use the XKB plane
  // as a reliable fallback: Flutter's key event system accepts
  //   0x00100000000 | xkb_keysym
  // for all non-Unicode keys (matching the behavior of the GTK embedder).
  return UINT64_C(0x100000000) | static_cast<uint64_t>(keysym);
}

}  // namespace key_mapping
