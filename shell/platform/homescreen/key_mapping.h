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

#pragma once

#include <cstdint>
#include <xkbcommon/xkbcommon.h>

namespace key_mapping {

// Converts an XKB scancode (Linux evdev + 8) to a Flutter physical key code.
// Physical keys use the USB HID plane: 0x00700000 | hid_usage_id.
// Unmapped keys fall back to the Linux code plane: 0x00600000 | evdev_code.
uint64_t XkbScancodeToPhysicalKey(uint32_t xkb_scancode);

// Converts an XKB keysym + UTF-32 codepoint to a Flutter logical key code.
// Printable characters map to their Unicode codepoint.
// Non-printable keys map using the XKB plane: 0x00100000000 | keysym, or a
// curated HID-based logical value for well-known keys.
uint64_t KeysymToLogicalKey(xkb_keysym_t keysym, uint32_t utf32);

}  // namespace key_mapping
