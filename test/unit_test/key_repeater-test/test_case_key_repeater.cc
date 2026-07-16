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

#include <poll.h>

#include <atomic>
#include <memory>
#include <string>

#include "gtest/gtest.h"

#include "input/key_repeater.h"
#include "input/xkb_keyboard.h"

using homescreen::input::KeyRepeater;
using homescreen::input::RepeatConfig;
using homescreen::input::XkbKeyboard;

// evdev keycodes.
static constexpr uint32_t kKeyA = 30;
static constexpr uint32_t kKeyLeftShift = 42;  // non-repeating modifier

// Short repeat config to keep tests fast: 80 ms initial delay, 40 ms interval.
static RepeatConfig FastConfig() {
  return RepeatConfig{/*delay_ms=*/80, /*interval_ms=*/40};
}

// Poll the fd for POLLIN with a timeout; returns true if readable.
static bool PollReadable(int fd, int timeout_ms) {
  pollfd p{fd, POLLIN, 0};
  return ::poll(&p, 1, timeout_ms) > 0 && (p.revents & POLLIN) != 0;
}

// ---------------------------------------------------------------------------
// Create()
// ---------------------------------------------------------------------------

TEST(KeyRepeater, Create_NullKeyboard_ReturnsNullptr) {
  EXPECT_EQ(KeyRepeater::Create(nullptr), nullptr);
}

TEST(KeyRepeater, Create_ValidKeyboard_Succeeds) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  auto rep = KeyRepeater::Create(kb.get(), FastConfig());
  EXPECT_NE(rep, nullptr);
}

// ---------------------------------------------------------------------------
// fd()
// ---------------------------------------------------------------------------

TEST(KeyRepeater, Fd_IsValid) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  auto rep = KeyRepeater::Create(kb.get(), FastConfig());
  ASSERT_NE(rep, nullptr);
  EXPECT_GE(rep->fd(), 0);
}

// ---------------------------------------------------------------------------
// OnKey() — non-repeating key does NOT arm the timer
// ---------------------------------------------------------------------------

TEST(KeyRepeater, OnKey_ShiftKey_DoesNotArmTimer) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  auto rep = KeyRepeater::Create(kb.get(), FastConfig());
  ASSERT_NE(rep, nullptr);

  rep->OnKey(kKeyLeftShift, /*pressed=*/true);
  // Shift is not repeatable: the timer must NOT fire within 200 ms.
  EXPECT_FALSE(PollReadable(rep->fd(), 200));
}

// ---------------------------------------------------------------------------
// OnKey() — repeatable key DOES arm the timer
// ---------------------------------------------------------------------------

TEST(KeyRepeater, OnKey_LetterKey_ArmsTimer) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  auto rep = KeyRepeater::Create(kb.get(), FastConfig());
  ASSERT_NE(rep, nullptr);

  rep->OnKey(kKeyA, true);
  // Timer should fire after the 80 ms initial delay; allow 500 ms.
  EXPECT_TRUE(PollReadable(rep->fd(), 500));
}

// ---------------------------------------------------------------------------
// Dispatch() — calls handler exactly once per poll-readable tick
// ---------------------------------------------------------------------------

TEST(KeyRepeater, Dispatch_AfterArm_CallsHandlerOnce) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  auto rep = KeyRepeater::Create(kb.get(), FastConfig());
  ASSERT_NE(rep, nullptr);

  std::atomic<int> call_count{0};
  rep->SetHandler([&](uint32_t /*evdev*/, xkb_keysym_t /*sym*/,
                      const std::string& /*utf8*/) { ++call_count; });

  rep->OnKey(kKeyA, true);
  ASSERT_TRUE(PollReadable(rep->fd(), 500));
  rep->Dispatch();
  EXPECT_EQ(call_count.load(), 1);
}

// ---------------------------------------------------------------------------
// Dispatch() — no handler set → no crash
// ---------------------------------------------------------------------------

TEST(KeyRepeater, Dispatch_NoHandler_DoesNotCrash) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  auto rep = KeyRepeater::Create(kb.get(), FastConfig());
  ASSERT_NE(rep, nullptr);

  rep->OnKey(kKeyA, true);
  if (PollReadable(rep->fd(), 500)) {
    // No handler installed — must not crash.
    rep->Dispatch();
  }
}

// ---------------------------------------------------------------------------
// OnKey() — releasing the held key disarms the timer
// ---------------------------------------------------------------------------

TEST(KeyRepeater, OnKey_Release_DisarmsTimer) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  auto rep = KeyRepeater::Create(kb.get(), FastConfig());
  ASSERT_NE(rep, nullptr);

  std::atomic<int> call_count{0};
  rep->SetHandler(
      [&](uint32_t, xkb_keysym_t, const std::string&) { ++call_count; });

  rep->OnKey(kKeyA, true);
  rep->OnKey(kKeyA, false);  // release immediately — disarms before first tick
  // Timer must NOT fire: wait 200 ms (well under the 80 ms delay but the
  // disarm should prevent any expiration).  If the timerfd already fired
  // between arm and disarm, Dispatch is a no-op because is_held_ is false.
  const bool fired = PollReadable(rep->fd(), 200);
  if (fired) {
    rep->Dispatch();  // safe: is_held_ is false → handler not called
  }
  EXPECT_EQ(call_count.load(), 0);
}

// ---------------------------------------------------------------------------
// Cancel() — clears the held key
// ---------------------------------------------------------------------------

TEST(KeyRepeater, Cancel_ClearsHeldKey) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  auto rep = KeyRepeater::Create(kb.get(), FastConfig());
  ASSERT_NE(rep, nullptr);

  rep->OnKey(kKeyA, true);
  EXPECT_EQ(rep->held_key(), kKeyA);
  rep->Cancel();
  EXPECT_EQ(rep->held_key(), 0u);
}

// ---------------------------------------------------------------------------
// SetHandler() — replacement handler fires on next Dispatch
// ---------------------------------------------------------------------------

TEST(KeyRepeater, SetHandler_NewHandler_Fires) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  auto rep = KeyRepeater::Create(kb.get(), FastConfig());
  ASSERT_NE(rep, nullptr);

  std::atomic<int> old_count{0};
  std::atomic<int> new_count{0};

  rep->SetHandler(
      [&](uint32_t, xkb_keysym_t, const std::string&) { ++old_count; });
  rep->OnKey(kKeyA, true);
  // Replace the handler before the first tick.
  rep->SetHandler(
      [&](uint32_t, xkb_keysym_t, const std::string&) { ++new_count; });
  ASSERT_TRUE(PollReadable(rep->fd(), 500));
  rep->Dispatch();
  EXPECT_EQ(old_count.load(), 0);
  EXPECT_EQ(new_count.load(), 1);
}

// ---------------------------------------------------------------------------
// Second repeatable key replaces the first
// ---------------------------------------------------------------------------

TEST(KeyRepeater, OnKey_SecondRepeatKey_ReplacesFirst) {
  auto kb = XkbKeyboard::Create({});
  ASSERT_NE(kb, nullptr);
  auto rep = KeyRepeater::Create(kb.get(), FastConfig());
  ASSERT_NE(rep, nullptr);

  const uint32_t kKeyB = 48;  // evdev KEY_B

  rep->OnKey(kKeyA, true);
  rep->OnKey(kKeyB, true);  // replaces held key
  EXPECT_EQ(rep->held_key(), kKeyB);
}
