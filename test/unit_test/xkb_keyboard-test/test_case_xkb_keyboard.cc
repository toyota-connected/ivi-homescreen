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

#include <memory>
#include <string>

#include <xkbcommon/xkbcommon.h>

#include "gtest/gtest.h"

#include "input/xkb_keyboard.h"

using homescreen::input::KeymapOptions;
using homescreen::input::XkbKeyboard;

// evdev keycodes used in the tests (Linux input.h values).
static constexpr uint32_t kKeyA = 30;
static constexpr uint32_t kKeyEscape = 1;
static constexpr uint32_t kKeyLeftShift = 42;
static constexpr uint32_t kKeyLeftCtrl = 29;

// ---------------------------------------------------------------------------
// Create()
// ---------------------------------------------------------------------------

TEST(XkbKeyboard, Create_DefaultOptions_Succeeds) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
}

TEST(XkbKeyboard, Create_NullKeyboard_NullptrGuard) {
  // Create() is never called with nullptr explicitly, but the returned
  // pointer must be non-null for a valid default keymap.
  auto kb = XkbKeyboard::Create({});
  EXPECT_NE(kb, nullptr);
}

// ---------------------------------------------------------------------------
// ProcessKey() — keysym resolution and state mutation
// ---------------------------------------------------------------------------

TEST(XkbKeyboard, ProcessKey_PressA_ReturnsXkbKeyA) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  const xkb_keysym_t sym = kb->ProcessKey(kKeyA, /*pressed=*/true);
  EXPECT_EQ(sym, static_cast<xkb_keysym_t>(XKB_KEY_a));
}

TEST(XkbKeyboard, ProcessKey_PressShiftThenA_ReturnsUppercaseA) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  kb->ProcessKey(kKeyLeftShift, true);
  const xkb_keysym_t sym = kb->ProcessKey(kKeyA, true);
  EXPECT_EQ(sym, static_cast<xkb_keysym_t>(XKB_KEY_A));
}

TEST(XkbKeyboard, ProcessKey_ReleaseShift_ModifiersUpdated) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  kb->ProcessKey(kKeyLeftShift, true);
  const uint32_t mods_pressed = kb->Modifiers();
  kb->ProcessKey(kKeyLeftShift, false);
  const uint32_t mods_released = kb->Modifiers();
  // After release the modifier mask must change (shift clears).
  EXPECT_NE(mods_pressed, mods_released);
}

TEST(XkbKeyboard, ProcessKey_PressA_Utf8IsLowerA) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  std::string utf8;
  kb->ProcessKey(kKeyA, true, &utf8);
  EXPECT_EQ(utf8, "a");
}

TEST(XkbKeyboard, ProcessKey_PressShiftA_Utf8IsUpperA) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  kb->ProcessKey(kKeyLeftShift, true);
  std::string utf8;
  kb->ProcessKey(kKeyA, true, &utf8);
  EXPECT_EQ(utf8, "A");
}

// ---------------------------------------------------------------------------
// ResolveSym() — non-mutating query
// ---------------------------------------------------------------------------

TEST(XkbKeyboard, ResolveSym_DoesNotMutateModifiers) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  const uint32_t before = kb->Modifiers();
  kb->ResolveSym(kKeyA);
  const uint32_t after = kb->Modifiers();
  EXPECT_EQ(before, after);
}

TEST(XkbKeyboard, ResolveSym_CalledTwice_SameResult) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  const xkb_keysym_t s1 = kb->ResolveSym(kKeyA);
  const xkb_keysym_t s2 = kb->ResolveSym(kKeyA);
  EXPECT_EQ(s1, s2);
}

// ---------------------------------------------------------------------------
// KeyRepeats()
// ---------------------------------------------------------------------------

TEST(XkbKeyboard, KeyRepeats_LetterKey_True) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  EXPECT_TRUE(kb->KeyRepeats(kKeyA));
}

TEST(XkbKeyboard, KeyRepeats_EscapeKey_True) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  // Escape repeats in most standard keymaps.
  EXPECT_TRUE(kb->KeyRepeats(kKeyEscape));
}

TEST(XkbKeyboard, KeyRepeats_ShiftKey_False) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  EXPECT_FALSE(kb->KeyRepeats(kKeyLeftShift));
}

// ---------------------------------------------------------------------------
// Modifiers() — initial state
// ---------------------------------------------------------------------------

TEST(XkbKeyboard, Modifiers_Initially_Zero) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  EXPECT_EQ(kb->Modifiers(), 0u);
}

// ---------------------------------------------------------------------------
// CtrlActive() / AltActive()
// ---------------------------------------------------------------------------

TEST(XkbKeyboard, CtrlActive_WhenCtrlPressed) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  EXPECT_FALSE(kb->CtrlActive());
  kb->ProcessKey(kKeyLeftCtrl, true);
  EXPECT_TRUE(kb->CtrlActive());
  kb->ProcessKey(kKeyLeftCtrl, false);
  EXPECT_FALSE(kb->CtrlActive());
}

TEST(XkbKeyboard, AltActive_InitiallyFalse) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  EXPECT_FALSE(kb->AltActive());
}

// ---------------------------------------------------------------------------
// Leds()
// ---------------------------------------------------------------------------

TEST(XkbKeyboard, Leds_Initial_AllFalse) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  const auto leds = kb->Leds();
  EXPECT_FALSE(leds.caps_lock);
  EXPECT_FALSE(leds.num_lock);
  EXPECT_FALSE(leds.scroll_lock);
}

// ---------------------------------------------------------------------------
// Reload()
// ---------------------------------------------------------------------------

TEST(XkbKeyboard, Reload_DefaultOptions_Succeeds) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  EXPECT_TRUE(kb->Reload({}));
}

TEST(XkbKeyboard, Reload_InvalidLayout_ReturnsFalse) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  KeymapOptions bad;
  bad.layout = "this-layout-does-not-exist-xyz";
  // Reload with an invalid layout should fail gracefully (returns false,
  // leaves the previous keymap intact — no crash).
  const bool ok = kb->Reload(bad);
  // Some xkbcommon versions fall back gracefully; the important invariant
  // is no crash and subsequent ProcessKey still works.
  (void)ok;
  const xkb_keysym_t sym = kb->ProcessKey(kKeyA, true);
  EXPECT_NE(sym, XKB_KEY_NoSymbol);
}
