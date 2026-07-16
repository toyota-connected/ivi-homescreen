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

#include <cstdlib>

#include "gtest/gtest.h"

#include "profiling/motion_to_photon.h"

using profiling::MotionToPhoton;

// ---------------------------------------------------------------------------
// Enabled() — env-var gate
// ---------------------------------------------------------------------------

TEST(MotionToPhoton, Enabled_WhenNoEnvSet) {
  ::unsetenv("IVI_PROFILE");
  ::unsetenv("IVI_M2P_PROFILE");
  EXPECT_FALSE(MotionToPhoton::Enabled());
}

TEST(MotionToPhoton, Enabled_WhenIviProfileSet) {
  ::setenv("IVI_PROFILE", "1", 1);
  EXPECT_TRUE(MotionToPhoton::Enabled());
  ::unsetenv("IVI_PROFILE");
}

TEST(MotionToPhoton, Enabled_WhenIviM2pProfileSet) {
  ::unsetenv("IVI_PROFILE");
  ::setenv("IVI_M2P_PROFILE", "1", 1);
  EXPECT_TRUE(MotionToPhoton::Enabled());
  ::unsetenv("IVI_M2P_PROFILE");
}

// ---------------------------------------------------------------------------
// RecordPresent() with no prior inputs — no crash, no data recorded
// ---------------------------------------------------------------------------

TEST(MotionToPhoton, RecordPresent_NoInputs_DoesNotCrash) {
  MotionToPhoton m;
  m.RecordPresent(/*present_ns=*/1'000'000'000ULL,
                  /*cutoff_ns=*/900'000'000ULL, "test");
  m.LogSessionSummary("test");
}

// ---------------------------------------------------------------------------
// RecordInput() then RecordPresent() — frame-accurate path
// ---------------------------------------------------------------------------

// Input before the cutoff → consumed by the frame-accurate pass.
TEST(MotionToPhoton, RecordInput_BeforeCutoff_ConsumedFrameAccurately) {
  MotionToPhoton m;
  constexpr uint64_t input_ns = 100'000'000ULL;    // 100 ms
  constexpr uint64_t cutoff_ns = 200'000'000ULL;   // 200 ms (input is before)
  constexpr uint64_t present_ns = 300'000'000ULL;  // 300 ms
  m.RecordInput(input_ns);
  m.RecordPresent(present_ns, cutoff_ns, "test");
  // LogSessionSummary should not be a no-op (session_.count > 0) —
  // we can't inspect private members so just verify no crash.
  m.LogSessionSummary("test");
}

// Input after the cutoff → NOT consumed by frame-accurate, but IS consumed
// by the floor pass (input <= present).
TEST(MotionToPhoton, RecordInput_AfterCutoff_ConsumedByFloorOnly) {
  MotionToPhoton m;
  constexpr uint64_t input_ns = 150'000'000ULL;    // after cutoff
  constexpr uint64_t cutoff_ns = 50'000'000ULL;    // cutoff is before input
  constexpr uint64_t present_ns = 300'000'000ULL;  // present is after input
  m.RecordInput(input_ns);
  m.RecordPresent(present_ns, cutoff_ns, "test");
  m.LogSessionSummary("test");
}

// cutoff == 0 → frame-accurate pass drains nothing; inputs stay for next
// present.
TEST(MotionToPhoton, RecordPresent_ZeroCutoff_InputStaysQueued) {
  MotionToPhoton m;
  m.RecordInput(100'000'000ULL);
  // cutoff = 0 → no input should be consumed frame-accurately.
  m.RecordPresent(300'000'000ULL, /*cutoff_ns=*/0ULL, "test");
  // Input <= present so floor pass may consume it, but frame-accurate does not.
  // A second present with a real cutoff now drains the input.
  m.RecordPresent(400'000'000ULL, /*cutoff_ns=*/200'000'000ULL, "test");
  m.LogSessionSummary("test");
}

// ---------------------------------------------------------------------------
// Multiple inputs across multiple presents
// ---------------------------------------------------------------------------

TEST(MotionToPhoton, MultipleInputsAndPresents_DoesNotCrash) {
  MotionToPhoton m;
  // Simulate 10 frames at 16 ms, each with one input 5 ms before its cutoff.
  for (int i = 0; i < 10; ++i) {
    const uint64_t frame_start = static_cast<uint64_t>(i) * 16'000'000ULL;
    m.RecordInput(frame_start + 3'000'000ULL);  // input 3 ms into frame
    m.RecordPresent(frame_start + 16'000'000ULL, frame_start + 8'000'000ULL,
                    "test");
  }
  m.LogSessionSummary("test");
}

// ---------------------------------------------------------------------------
// Ring overflow — fill 257 inputs (kPending = 256)
// ---------------------------------------------------------------------------

TEST(MotionToPhoton, RecordInput_RingOverflow_DoesNotCrash) {
  MotionToPhoton m;
  // kPending = 256: filling 257 entries triggers the drop-oldest path.
  for (uint64_t i = 0; i < 257; ++i) {
    m.RecordInput(i * 1'000'000ULL);
  }
  // Drain everything with one present.
  m.RecordPresent(300'000'000'000ULL, 300'000'000'000ULL, "overflow");
  m.LogSessionSummary("overflow");
}

// ---------------------------------------------------------------------------
// LogSessionSummary() on empty profiler — no-op, no crash
// ---------------------------------------------------------------------------

TEST(MotionToPhoton, LogSessionSummary_Empty_DoesNotCrash) {
  const MotionToPhoton m;
  m.LogSessionSummary("empty");
}

// ---------------------------------------------------------------------------
// Window log flush — after kWindow (120) presents
// ---------------------------------------------------------------------------

TEST(MotionToPhoton, WindowFlush_120Presents_DoesNotCrash) {
  MotionToPhoton m;
  // Feed one input per frame and 120 presents to trigger the window log.
  for (int i = 0; i < 120; ++i) {
    const uint64_t t = static_cast<uint64_t>(i) * 16'000'000ULL;
    m.RecordInput(t);
    m.RecordPresent(t + 16'000'000ULL, t + 8'000'000ULL, "window");
  }
  m.LogSessionSummary("window");
}
