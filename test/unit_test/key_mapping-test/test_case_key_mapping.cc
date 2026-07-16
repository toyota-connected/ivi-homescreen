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

// Unit tests for key_mapping::XkbScancodeToPhysicalKey and
// key_mapping::KeysymToLogicalKey.
//
// Both functions are pure table lookups — no Wayland, EGL, or Flutter engine
// dependency.  The xkbcommon keysym constants are header-only (they map to
// integer literals); only the <xkbcommon/xkbcommon.h> header is required.

#include <cstdint>

#include <gtest/gtest.h>
#include <xkbcommon/xkbcommon.h>

#include "shell/platform/homescreen/key_mapping.h"

// evdev key codes referenced below (same values as <linux/input-event-codes.h>)
static constexpr uint32_t kEvdevEsc = 1;
static constexpr uint32_t kEvdevEnter = 28;
static constexpr uint32_t kEvdevA = 30;
static constexpr uint32_t kEvdevF5 = 63;
// An evdev code whose kEvdevToHidMap entry is 0x00 (unmapped to HID).
// evdev 128 (KEY_STOP) has no HID mapping in the project's table.
static constexpr uint32_t kEvdevUnmapped = 128;
// xkb_scancode = evdev + 8
static constexpr uint32_t kXkbScancode(uint32_t evdev) {
  return evdev + 8;
}

// ============================================================
// XkbScancodeToPhysicalKey
// ============================================================

TEST(KeyMapping, Scancode_LessThan8_ReturnsZero) {
  // Guard: xkb scancodes below 8 are invalid (no valid evdev code maps there).
  for (uint32_t s = 0; s < 8; ++s) {
    EXPECT_EQ(key_mapping::XkbScancodeToPhysicalKey(s), UINT64_C(0))
        << "scancode=" << s;
  }
}

TEST(KeyMapping, Scancode_Esc_ReturnsHidPlane) {
  // KEY_ESC (evdev 1) → HID 0x29 → physical 0x00070029
  EXPECT_EQ(key_mapping::XkbScancodeToPhysicalKey(kXkbScancode(kEvdevEsc)),
            UINT64_C(0x00070029));
}

TEST(KeyMapping, Scancode_Enter_ReturnsHidPlane) {
  // KEY_ENTER (evdev 28) → HID 0x28 → physical 0x00070028
  EXPECT_EQ(key_mapping::XkbScancodeToPhysicalKey(kXkbScancode(kEvdevEnter)),
            UINT64_C(0x00070028));
}

TEST(KeyMapping, Scancode_A_ReturnsHidPlane) {
  // KEY_A (evdev 30) → HID 0x04 → physical 0x00070004
  EXPECT_EQ(key_mapping::XkbScancodeToPhysicalKey(kXkbScancode(kEvdevA)),
            UINT64_C(0x00070004));
}

TEST(KeyMapping, Scancode_F5_ReturnsHidPlane) {
  // KEY_F5 (evdev 63) → HID 0x3e → physical 0x0007003e
  EXPECT_EQ(key_mapping::XkbScancodeToPhysicalKey(kXkbScancode(kEvdevF5)),
            UINT64_C(0x0007003e));
}

TEST(KeyMapping, Scancode_Unmapped_ReturnsLinuxPlane) {
  // kEvdevUnmapped (128) has no HID mapping → Linux plane 0x00600000 | evdev
  const uint64_t expected = UINT64_C(0x00600000) | kEvdevUnmapped;
  EXPECT_EQ(key_mapping::XkbScancodeToPhysicalKey(kXkbScancode(kEvdevUnmapped)),
            expected);
}

TEST(KeyMapping, Scancode_HighEvdev_ReturnsLinuxPlane) {
  // evdev codes > 255 always fall back to the Linux plane.
  static constexpr uint32_t kHighEvdev = 300;
  const uint64_t expected = UINT64_C(0x00600000) | kHighEvdev;
  EXPECT_EQ(key_mapping::XkbScancodeToPhysicalKey(kXkbScancode(kHighEvdev)),
            expected);
}

// ============================================================
// KeysymToLogicalKey
// ============================================================

TEST(KeyMapping, Logical_PrintableAscii_ReturnsUtf32) {
  // Printable: 'A' (utf32 0x41) → logical 0x41
  EXPECT_EQ(key_mapping::KeysymToLogicalKey(XKB_KEY_A, 0x41), UINT64_C(0x41));
  EXPECT_EQ(key_mapping::KeysymToLogicalKey(XKB_KEY_a, 0x61), UINT64_C(0x61));
  EXPECT_EQ(key_mapping::KeysymToLogicalKey(XKB_KEY_space, 0x20),
            UINT64_C(0x20));
}

TEST(KeyMapping, Logical_Del_NotPrintable_FallsToXkbPlane) {
  // DEL (utf32 0x7f) is explicitly excluded from the printable range.
  // It is not in kKeysymToLogical either → XKB plane fallback.
  const uint64_t result = key_mapping::KeysymToLogicalKey(XKB_KEY_Delete, 0x7f);
  // Either the keysym table maps it (0x10000007f) or the XKB plane does
  // (0x100000000 | XKB_KEY_Delete). Both are non-zero and ≥ 0x100000000.
  EXPECT_GE(result, UINT64_C(0x100000000));
}

TEST(KeyMapping, Logical_Return_ReturnsMappedId) {
  // XKB_KEY_Return is in kKeysymToLogical → 0x10000000d (Flutter enter key)
  EXPECT_EQ(key_mapping::KeysymToLogicalKey(XKB_KEY_Return, 0),
            UINT64_C(0x10000000d));
}

TEST(KeyMapping, Logical_LeftArrow_ReturnsMappedId) {
  EXPECT_EQ(key_mapping::KeysymToLogicalKey(XKB_KEY_Left, 0),
            UINT64_C(0x100000302));
}

TEST(KeyMapping, Logical_F5_ReturnsMappedId) {
  EXPECT_EQ(key_mapping::KeysymToLogicalKey(XKB_KEY_F5, 0),
            UINT64_C(0x100000805));
}

TEST(KeyMapping, Logical_Escape_ReturnsMappedId) {
  EXPECT_EQ(key_mapping::KeysymToLogicalKey(XKB_KEY_Escape, 0),
            UINT64_C(0x10000001b));
}

TEST(KeyMapping, Logical_CtrlL_ReturnsMappedId) {
  // Modifier key in kKeysymToLogical.
  EXPECT_EQ(key_mapping::KeysymToLogicalKey(XKB_KEY_Control_L, 0),
            UINT64_C(0x200000100));
}

TEST(KeyMapping, Logical_UnknownKeysym_ReturnsXkbPlane) {
  // A keysym that is definitely not in the table; utf32 = 0.
  static constexpr xkb_keysym_t kUnknown = 0xDEAD;
  EXPECT_EQ(key_mapping::KeysymToLogicalKey(kUnknown, 0),
            UINT64_C(0x100000000) | static_cast<uint64_t>(kUnknown));
}
