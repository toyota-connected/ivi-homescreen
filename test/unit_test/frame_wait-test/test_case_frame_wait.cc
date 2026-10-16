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

// The wait policy behind the dma-buf path's refresh pacing. What is worth
// pinning is when the wait may end early (a free slot, a known refresh, a
// prior commit, a recent callback -- all four) and that early never means two
// commits inside one repaint, a wait past the cap, or an occluded surface
// rendering at the refresh rate.

#include <gtest/gtest.h>

#include <chrono>

#include "backend/wayland_vulkan/frame_wait.h"

namespace {

using std::chrono::milliseconds;
using std::chrono::nanoseconds;
using Clock = std::chrono::steady_clock;
using wl_vulkan::FrameWaitDeadline;
using wl_vulkan::kFrameWaitCap;

constexpr uint32_t k60Hz = 16'666'667;
const Clock::time_point kNow = Clock::time_point{} + std::chrono::seconds(10);
// A callback that fired within the last refresh: callbacks are flowing.
const Clock::time_point kRecent = kNow - milliseconds(10);

TEST(FrameWait, NoFreeSlotWaitsForTheCallback) {
  EXPECT_EQ(
      FrameWaitDeadline(kNow, false, kNow - milliseconds(30), kRecent, k60Hz),
      kNow + kFrameWaitCap);
}

TEST(FrameWait, UnknownRefreshWaitsForTheCallback) {
  EXPECT_EQ(FrameWaitDeadline(kNow, true, kNow - milliseconds(30), kRecent, 0),
            kNow + kFrameWaitCap);
}

TEST(FrameWait, FirstFrameWaitsForTheCallback) {
  EXPECT_EQ(FrameWaitDeadline(kNow, true, std::nullopt, kRecent, k60Hz),
            kNow + kFrameWaitCap);
}

TEST(FrameWait, OccludedSurfaceWaitsForTheCap) {
  // Callbacks stopped two refreshes ago; old buffers are still released.
  EXPECT_EQ(FrameWaitDeadline(kNow, true, kNow - milliseconds(4),
                              kNow - milliseconds(34), k60Hz),
            kNow + kFrameWaitCap);
}

TEST(FrameWait, NoCallbackYetWaitsForTheCallback) {
  EXPECT_EQ(FrameWaitDeadline(kNow, true, kNow - milliseconds(4), std::nullopt,
                              k60Hz),
            kNow + kFrameWaitCap);
}

TEST(FrameWait, LateCallbackStillCounts) {
  // The last callback landed a refresh and a half ago: late, but flowing.
  const Clock::time_point commit = kNow - milliseconds(4);
  EXPECT_EQ(
      FrameWaitDeadline(kNow, true, commit, kNow - milliseconds(25), k60Hz),
      commit + nanoseconds(12'500'000));
}

TEST(FrameWait, FreeSlotEndsThreeQuartersOfARefreshAfterTheCommit) {
  const Clock::time_point commit = kNow - milliseconds(4);
  EXPECT_EQ(FrameWaitDeadline(kNow, true, commit, kRecent, k60Hz),
            commit + nanoseconds(12'500'000));
}

TEST(FrameWait, NeverAllowsTwoCommitsInOneRepaint) {
  // However the frame start drifts, the next commit is more than half a
  // refresh after the last one.
  for (int ms = 0; ms <= 40; ++ms) {
    const Clock::time_point commit = kNow - milliseconds(ms);
    const auto deadline = FrameWaitDeadline(kNow, true, commit, kRecent, k60Hz);
    EXPECT_GT(deadline - commit, nanoseconds(k60Hz / 2)) << ms << " ms";
  }
}

TEST(FrameWait, AnOldCommitDoesNotWait) {
  EXPECT_EQ(
      FrameWaitDeadline(kNow, true, kNow - milliseconds(30), kRecent, k60Hz),
      kNow);
}

TEST(FrameWait, NeverPastTheCap) {
  // A refresh period long enough to push the spacing past the cap.
  EXPECT_EQ(FrameWaitDeadline(kNow, true, kNow, kRecent, 1'000'000'000),
            kNow + kFrameWaitCap);
}

}  // namespace
