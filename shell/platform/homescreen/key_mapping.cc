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

/**
 * evdev keycode → USB HID usage (page 0x07 / Keyboard/Keypad page).
 * Only keys that require an explicit mapping are listed; all others fall
 * through to the Linux-plane fallback (0x00600000 | evdev_code).
 *
 * evdev codes come from <linux/input-event-codes.h>; USB HID usages from
 * https://usb.org/sites/default/files/hut1_3_0.pdf §10.
 *
 * The table has been remapped to uint8 key space for O(1) lookup.
 * Empty lines between sections indicate group discontinuity
 * (and should be left for readability).
 *
 * Usage rules:
 *  - EVDEV ID (clamped to 0xFF) = array key
 *  - HID ID = array value
 *  - 0x00 -> unmapped (maps to Linux plane fallback)
 */
// clang-format off
static const uint8_t kEvdevToHidMap[] = {
  /*   0 */ 0x00,  // KEY_RESERVED  → (none)

  /*   1 */ 0x29,  // KEY_ESC       → Escape

  // Digits — evdev 2..11 (KEY_1..KEY_0)
  /*   2 */ 0x1e,  // KEY_1
  /*   3 */ 0x1f,  // KEY_2
  /*   4 */ 0x20,  // KEY_3
  /*   5 */ 0x21,  // KEY_4
  /*   6 */ 0x22,  // KEY_5
  /*   7 */ 0x23,  // KEY_6
  /*   8 */ 0x24,  // KEY_7
  /*   9 */ 0x25,  // KEY_8
  /*  10 */ 0x26,  // KEY_9
  /*  11 */ 0x27,  // KEY_0

  /*  12 */ 0x2d,  // KEY_MINUS
  /*  13 */ 0x2e,  // KEY_EQUAL

  /*  14 */ 0x2a,  // KEY_BACKSPACE → Backspace
  /*  15 */ 0x2b,  // KEY_TAB       → Tab

  // Alpha keys — evdev codes from <linux/input-event-codes.h>,
  /*  16 */ 0x14,  // KEY_Q → HID q
  /*  17 */ 0x1a,  // KEY_W → HID w
  /*  18 */ 0x08,  // KEY_E → HID e
  /*  19 */ 0x15,  // KEY_R → HID r
  /*  20 */ 0x17,  // KEY_T → HID t
  /*  21 */ 0x1c,  // KEY_Y → HID y
  /*  22 */ 0x18,  // KEY_U → HID u
  /*  23 */ 0x0c,  // KEY_I → HID i
  /*  24 */ 0x12,  // KEY_O → HID o
  /*  25 */ 0x13,  // KEY_P → HID p

  /*  26 */ 0x2f,  // KEY_LEFTBRACE
  /*  27 */ 0x30,  // KEY_RIGHTBRACE
  /*  28 */ 0x28,  // KEY_ENTER     → Return

  /*  29 */ 0xe0,  // KEY_LEFTCTRL

  /*  30 */ 0x04,  // KEY_A → HID a
  /*  31 */ 0x16,  // KEY_S → HID s
  /*  32 */ 0x07,  // KEY_D → HID d
  /*  33 */ 0x09,  // KEY_F → HID f
  /*  34 */ 0x0a,  // KEY_G → HID g
  /*  35 */ 0x0b,  // KEY_H → HID h
  /*  36 */ 0x0d,  // KEY_J → HID j
  /*  37 */ 0x0e,  // KEY_K → HID k
  /*  38 */ 0x0f,  // KEY_L → HID l

  /*  39 */ 0x33,  // KEY_SEMICOLON
  /*  40 */ 0x34,  // KEY_APOSTROPHE
  /*  41 */ 0x35,  // KEY_GRAVE

  /*  42 */ 0xe1,  // KEY_LEFTSHIFT

  /*  43 */ 0x31,  // KEY_BACKSLASH

  /*  44 */ 0x1d,  // KEY_Z → HID z
  /*  45 */ 0x1b,  // KEY_X → HID x
  /*  46 */ 0x06,  // KEY_C → HID c
  /*  47 */ 0x19,  // KEY_V → HID v
  /*  48 */ 0x05,  // KEY_B → HID b
  /*  49 */ 0x11,  // KEY_N → HID n
  /*  50 */ 0x10,  // KEY_M → HID m

  /*  51 */ 0x36,  // KEY_COMMA
  /*  52 */ 0x37,  // KEY_DOT
  /*  53 */ 0x38,  // KEY_SLASH

  /*  54 */ 0xe5,  // KEY_RIGHTSHIFT

  /*  55 */ 0x55,  // KEY_KPASTERISK

  /*  56 */ 0xe2,  // KEY_LEFTALT
  
  /*  57 */ 0x2c,  // KEY_SPACE     → Space
  /*  58 */ 0x39,  // KEY_CAPSLOCK  → Caps Lock

  // Function keys
  /*  59 */ 0x3a,  // KEY_F1
  /*  60 */ 0x3b,  // KEY_F2
  /*  61 */ 0x3c,  // KEY_F3
  /*  62 */ 0x3d,  // KEY_F4
  /*  63 */ 0x3e,  // KEY_F5
  /*  64 */ 0x3f,  // KEY_F6
  /*  65 */ 0x40,  // KEY_F7
  /*  66 */ 0x41,  // KEY_F8
  /*  67 */ 0x42,  // KEY_F9
  /*  68 */ 0x43,  // KEY_F10

  /*  69 */ 0x53,  // KEY_NUMLOCK
  /*  70 */ 0x47,  // KEY_SCROLLLOCK

  // Numpad
  /*  71 */ 0x5f,  // KEY_KP7
  /*  72 */ 0x60,  // KEY_KP8
  /*  73 */ 0x61,  // KEY_KP9
  /*  74 */ 0x56,  // KEY_KPMINUS
  /*  75 */ 0x5c,  // KEY_KP4
  /*  76 */ 0x5d,  // KEY_KP5
  /*  77 */ 0x5e,  // KEY_KP6
  /*  78 */ 0x57,  // KEY_KPPLUS
  /*  79 */ 0x59,  // KEY_KP1
  /*  80 */ 0x5a,  // KEY_KP2
  /*  81 */ 0x5b,  // KEY_KP3
  /*  82 */ 0x62,  // KEY_KP0
  /*  83 */ 0x63,  // KEY_KPDOT

  /* 84-86 = Unmapped */
  /*  84 */ 0x00,
  /*  85 */ 0x00,
  /*  86 */ 0x00,
  
  /*  87 */ 0x44,  // KEY_F11
  /*  88 */ 0x45,  // KEY_F12

  /* 89-95 = Unmapped */
  /*  89 */ 0x00,
  /*  90 */ 0x00,
  /*  91 */ 0x00,
  /*  92 */ 0x00,
  /*  93 */ 0x00,
  /*  94 */ 0x00,
  /*  95 */ 0x00,

  /*  96 */ 0x58,  // KEY_KPENTER

  /*  97 */ 0xe4,  // KEY_RIGHTCTRL

  /*  98 */ 0x54,  // KEY_KPSLASH

  /*  99 */ 0x46,  // KEY_SYSRQ / PrintScreen
  /* 100 */ 0xe6,  // KEY_RIGHTALT

  /* 101 */ 0x00,  // Unmapped

  // Navigation cluster
  /* 102 */ 0x4a,  // KEY_HOME
  /* 103 */ 0x52,  // KEY_UP
  /* 104 */ 0x4b,  // KEY_PAGEUP
  /* 105 */ 0x50,  // KEY_LEFT
  /* 106 */ 0x4f,  // KEY_RIGHT
  /* 107 */ 0x4d,  // KEY_END
  /* 108 */ 0x51,  // KEY_DOWN
  /* 109 */ 0x4e,  // KEY_PAGEDOWN
  /* 110 */ 0x49,  // KEY_INSERT
  /* 111 */ 0x4c,  // KEY_DELETE

  /* 112-118 = Unmapped */
  /* 112 */ 0x00,
  /* 113 */ 0x00,
  /* 114 */ 0x00,
  /* 115 */ 0x00,
  /* 116 */ 0x00,
  /* 117 */ 0x00,
  /* 118 */ 0x00,

  /* 119 */ 0x48,  // KEY_PAUSE

  /* 120-124 = Unmapped */
  /* 120 */ 0x00,
  /* 121 */ 0x00,
  /* 122 */ 0x00,
  /* 123 */ 0x00,
  /* 124 */ 0x00,

  /* 125 */ 0xe3,  // KEY_LEFTMETA
  /* 126 */ 0xe7,  // KEY_RIGHTMETA

  /* 127 */ 0x65,  // KEY_COMPOSE / Menu

  /* 128-182 = Unmapped */
  /* 128 */ 0x00,
  /* 129 */ 0x00,
  /* 130 */ 0x00,
  /* 131 */ 0x00,
  /* 132 */ 0x00,
  /* 133 */ 0x00,
  /* 134 */ 0x00,
  /* 135 */ 0x00,
  /* 136 */ 0x00,
  /* 137 */ 0x00,
  /* 138 */ 0x00,
  /* 139 */ 0x00,
  /* 140 */ 0x00,
  /* 141 */ 0x00,
  /* 142 */ 0x00,
  /* 143 */ 0x00,
  /* 144 */ 0x00,
  /* 145 */ 0x00,
  /* 146 */ 0x00,
  /* 147 */ 0x00,
  /* 148 */ 0x00,
  /* 149 */ 0x00,
  /* 150 */ 0x00,
  /* 151 */ 0x00,
  /* 152 */ 0x00,
  /* 153 */ 0x00,
  /* 154 */ 0x00,
  /* 155 */ 0x00,
  /* 156 */ 0x00,
  /* 157 */ 0x00,
  /* 158 */ 0x00,
  /* 159 */ 0x00,
  /* 160 */ 0x00,
  /* 161 */ 0x00,
  /* 162 */ 0x00,
  /* 163 */ 0x00,
  /* 164 */ 0x00,
  /* 165 */ 0x00,
  /* 166 */ 0x00,
  /* 167 */ 0x00,
  /* 168 */ 0x00,
  /* 169 */ 0x00,
  /* 170 */ 0x00,
  /* 171 */ 0x00,
  /* 172 */ 0x00,
  /* 173 */ 0x00,
  /* 174 */ 0x00,
  /* 175 */ 0x00,
  /* 176 */ 0x00,
  /* 177 */ 0x00,
  /* 178 */ 0x00,
  /* 179 */ 0x00,
  /* 180 */ 0x00,
  /* 181 */ 0x00,
  /* 182 */ 0x00,

  // Extended Function keys
  /* 183 */ 0x68,  // KEY_F13
  /* 184 */ 0x69,  // KEY_F14
  /* 185 */ 0x6a,  // KEY_F15
  /* 186 */ 0x6b,  // KEY_F16
  /* 187 */ 0x6c,  // KEY_F17
  /* 188 */ 0x6d,  // KEY_F18
  /* 189 */ 0x6e,  // KEY_F19
  /* 190 */ 0x6f,  // KEY_F20
  /* 191 */ 0x70,  // KEY_F21
  /* 192 */ 0x71,  // KEY_F22
  /* 193 */ 0x72,  // KEY_F23
  /* 194 */ 0x73,  // KEY_F24

  /* 195-255 = Unmapped */
  /* 195 */ 0x00,
  /* 196 */ 0x00,
  /* 197 */ 0x00,
  /* 198 */ 0x00,
  /* 199 */ 0x00,
  /* 200 */ 0x00,
  /* 201 */ 0x00,
  /* 202 */ 0x00,
  /* 203 */ 0x00,
  /* 204 */ 0x00,
  /* 205 */ 0x00,
  /* 206 */ 0x00,
  /* 207 */ 0x00,
  /* 208 */ 0x00,
  /* 209 */ 0x00,
  /* 210 */ 0x00,
  /* 211 */ 0x00,
  /* 212 */ 0x00,
  /* 213 */ 0x00,
  /* 214 */ 0x00,
  /* 215 */ 0x00,
  /* 216 */ 0x00,
  /* 217 */ 0x00,
  /* 218 */ 0x00,
  /* 219 */ 0x00,
  /* 220 */ 0x00,
  /* 221 */ 0x00,
  /* 222 */ 0x00,
  /* 223 */ 0x00,
  /* 224 */ 0x00,
  /* 225 */ 0x00,
  /* 226 */ 0x00,
  /* 227 */ 0x00,
  /* 228 */ 0x00,
  /* 229 */ 0x00,
  /* 230 */ 0x00,
  /* 231 */ 0x00,
  /* 232 */ 0x00,
  /* 233 */ 0x00,
  /* 234 */ 0x00,
  /* 235 */ 0x00,
  /* 236 */ 0x00,
  /* 237 */ 0x00,
  /* 238 */ 0x00,
  /* 239 */ 0x00,
  /* 240 */ 0x00,
  /* 241 */ 0x00,
  /* 242 */ 0x00,
  /* 243 */ 0x00,
  /* 244 */ 0x00,
  /* 245 */ 0x00,
  /* 246 */ 0x00,
  /* 247 */ 0x00,
  /* 248 */ 0x00,
  /* 249 */ 0x00,
  /* 250 */ 0x00,
  /* 251 */ 0x00,
  /* 252 */ 0x00,
  /* 253 */ 0x00,
  /* 254 */ 0x00,
  /* 255 */ 0x00,
};

/**
 * XKB keysym → Flutter logical key ID.
 * Values from flutter/packages/flutter/lib/src/services/keyboard_key.g.dart.
 * Only non-printable keys are listed; printable ones are handled by the
 * utf32 path in KeysymToLogicalKey.
 */
// clang-format off
static const std::unordered_map<xkb_keysym_t, uint64_t> kKeysymToLogical = {
  // Control / editing
  {XKB_KEY_BackSpace,    UINT64_C(0x100000008)},  // backspace
  {XKB_KEY_Tab,          UINT64_C(0x100000009)},  // tab
  {XKB_KEY_Return,       UINT64_C(0x10000000d)},  // enter
  {XKB_KEY_Escape,       UINT64_C(0x10000001b)},  // escape
  {XKB_KEY_Delete,       UINT64_C(0x10000007f)},  // delete

  // Navigation
  {XKB_KEY_Home,         UINT64_C(0x100000306)},
  {XKB_KEY_Left,         UINT64_C(0x100000302)},
  {XKB_KEY_Up,           UINT64_C(0x100000304)},
  {XKB_KEY_Right,        UINT64_C(0x100000303)},
  {XKB_KEY_Down,         UINT64_C(0x100000301)},
  {XKB_KEY_Page_Up,      UINT64_C(0x100000308)},
  {XKB_KEY_Page_Down,    UINT64_C(0x100000307)},
  {XKB_KEY_End,          UINT64_C(0x100000305)},
  {XKB_KEY_Insert,       UINT64_C(0x100000407)},

  // Numpad navigation (NumLock off)
  {XKB_KEY_KP_Home,      UINT64_C(0x100000306)},
  {XKB_KEY_KP_Left,      UINT64_C(0x100000302)},
  {XKB_KEY_KP_Up,        UINT64_C(0x100000304)},
  {XKB_KEY_KP_Right,     UINT64_C(0x100000303)},
  {XKB_KEY_KP_Down,      UINT64_C(0x100000301)},
  {XKB_KEY_KP_Page_Up,   UINT64_C(0x100000308)},
  {XKB_KEY_KP_Page_Down, UINT64_C(0x100000307)},
  {XKB_KEY_KP_End,       UINT64_C(0x100000305)},
  {XKB_KEY_KP_Insert,    UINT64_C(0x100000407)},
  {XKB_KEY_KP_Delete,    UINT64_C(0x10000007f)},
  {XKB_KEY_KP_Enter,     UINT64_C(0x10000000d)},

  // Lock keys
  {XKB_KEY_Caps_Lock,    UINT64_C(0x100000104)},
  {XKB_KEY_Num_Lock,     UINT64_C(0x10000010a)},
  {XKB_KEY_Scroll_Lock,  UINT64_C(0x10000010c)},

  // Modifier keys — use Flutter's modifier-plane IDs (0x200000xxx)
  {XKB_KEY_Control_L,    UINT64_C(0x200000100)},
  {XKB_KEY_Control_R,    UINT64_C(0x200000101)},
  {XKB_KEY_Shift_L,      UINT64_C(0x200000102)},
  {XKB_KEY_Shift_R,      UINT64_C(0x200000103)},
  {XKB_KEY_Alt_L,        UINT64_C(0x200000104)},
  {XKB_KEY_Alt_R,        UINT64_C(0x200000105)},
  {XKB_KEY_Meta_L,       UINT64_C(0x200000106)},
  {XKB_KEY_Meta_R,       UINT64_C(0x200000107)},
  {XKB_KEY_Super_L,      UINT64_C(0x200000106)},
  {XKB_KEY_Super_R,      UINT64_C(0x200000107)},

  // Function keys
  {XKB_KEY_F1,           UINT64_C(0x100000801)},
  {XKB_KEY_F2,           UINT64_C(0x100000802)},
  {XKB_KEY_F3,           UINT64_C(0x100000803)},
  {XKB_KEY_F4,           UINT64_C(0x100000804)},
  {XKB_KEY_F5,           UINT64_C(0x100000805)},
  {XKB_KEY_F6,           UINT64_C(0x100000806)},
  {XKB_KEY_F7,           UINT64_C(0x100000807)},
  {XKB_KEY_F8,           UINT64_C(0x100000808)},
  {XKB_KEY_F9,           UINT64_C(0x100000809)},
  {XKB_KEY_F10,          UINT64_C(0x10000080a)},
  {XKB_KEY_F11,          UINT64_C(0x10000080b)},
  {XKB_KEY_F12,          UINT64_C(0x10000080c)},
  {XKB_KEY_F13,          UINT64_C(0x10000080d)},
  {XKB_KEY_F14,          UINT64_C(0x10000080e)},
  {XKB_KEY_F15,          UINT64_C(0x10000080f)},
  {XKB_KEY_F16,          UINT64_C(0x100000810)},
  {XKB_KEY_F17,          UINT64_C(0x100000811)},
  {XKB_KEY_F18,          UINT64_C(0x100000812)},
  {XKB_KEY_F19,          UINT64_C(0x100000813)},
  {XKB_KEY_F20,          UINT64_C(0x100000814)},
  {XKB_KEY_F21,          UINT64_C(0x100000815)},
  {XKB_KEY_F22,          UINT64_C(0x100000816)},
  {XKB_KEY_F23,          UINT64_C(0x100000817)},
  {XKB_KEY_F24,          UINT64_C(0x100000818)},

  // Misc
  {XKB_KEY_Print,        UINT64_C(0x100000408)},
  {XKB_KEY_Sys_Req,      UINT64_C(0x100000408)},
  {XKB_KEY_Pause,        UINT64_C(0x100000409)},
};
// clang-format on

// ---------------------------------------------------------------------------

uint64_t XkbScancodeToPhysicalKey(const uint32_t xkb_scancode) {
  // xkb_scancode = evdev + 8; recover evdev.
  if (xkb_scancode < 8) {
    return 0;
  }
  const uint32_t evdev = xkb_scancode - 8;

  const uint8_t evdev8 = evdev & 0xff;  // Clamp to 0..255 for lookup.
  const uint32_t hidCode = kEvdevToHidMap[evdev8];
  if (hidCode != 0x00) {
    return UINT64_C(0x00070000) | hidCode;
  }
  // Fallback: Linux code plane.
  return UINT64_C(0x00600000) | evdev;
}

uint64_t KeysymToLogicalKey(const xkb_keysym_t keysym,
                            const uint32_t utf32,
                            const uint64_t /*physical*/) {
  // Printable Unicode characters: logical key == Unicode code point.
  if (utf32 >= 0x20 && utf32 != 0x7f) {
    return static_cast<uint64_t>(utf32);
  }

  // Look up exact Flutter logical key ID from the keysym table.
  const auto it = kKeysymToLogical.find(keysym);
  if (it != kKeysymToLogical.end()) {
    return it->second;
  }

  // Fallback: XKB plane — 0x100000000 | xkb_keysym.
  return UINT64_C(0x100000000) | static_cast<uint64_t>(keysym);
}

}  // namespace key_mapping
