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

// When a present composites: at the repaint window before the next vblank,
// counted from the last flip, or at once when there is nothing to count from.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>

#include "backend/common/repaint_window.h"

namespace {

constexpr uint64_t kPeriod = 16'666'667;
constexpr uint64_t kFlip = 1'000'000'000;
constexpr uint64_t kWindow = 5'000'000;
constexpr uint64_t kMs = 1'000'000;

}  // namespace

TEST(RepaintWindow, APresentEarlyInARefreshWaitsForTheWindow) {
  // Presented 1 ms after the flip: composite 5 ms before the next vblank.
  EXPECT_EQ(RepaintDelayNs(kFlip + kMs, kFlip, kPeriod, kWindow),
            kPeriod - kWindow - kMs);
}

TEST(RepaintWindow, APresentInsideTheWindowGoesAtOnce) {
  EXPECT_EQ(RepaintDelayNs(kFlip + kPeriod - kWindow, kFlip, kPeriod, kWindow),
            0U);
  EXPECT_EQ(RepaintDelayNs(kFlip + kPeriod - kMs, kFlip, kPeriod, kWindow), 0U);
}

TEST(RepaintWindow, VblanksAreCountedOnFromTheLastFlip) {
  // Idle for two refreshes, then a present 1 ms into the third.
  const uint64_t now = kFlip + 2 * kPeriod + kMs;
  EXPECT_EQ(RepaintDelayNs(now, kFlip, kPeriod, kWindow),
            kPeriod - kWindow - kMs);
}

TEST(RepaintWindow, AFlipTooLongAgoIsNotCountedFrom) {
  // The period is rounded, so its phase drifts: after a while, go at once.
  EXPECT_EQ(RepaintDelayNs(kFlip + 8 * kPeriod + kMs, kFlip, kPeriod, kWindow),
            0U);
  EXPECT_NE(RepaintDelayNs(kFlip + 7 * kPeriod + kMs, kFlip, kPeriod, kWindow),
            0U);
}

// The flip's event arrives at the start of the vblank and its stamp is the
// end (vc4: ~1 ms later). A present between the two was composited at once,
// as if there were no flip to count from, and the window did nothing.
TEST(RepaintWindow, APresentBeforeTheFlipStampWaitsForTheWindow) {
  EXPECT_EQ(RepaintDelayNs(kFlip - kMs, kFlip, kPeriod, kWindow),
            kPeriod - kWindow + kMs);
  EXPECT_EQ(TargetVblankNs(kFlip - kMs, kFlip, kPeriod), kFlip + kPeriod);
  EXPECT_EQ(JudgedTargetNs(kFlip - kMs, kFlip, kPeriod, kWindow),
            kFlip + kPeriod);
}

TEST(RepaintWindow, NothingToCountFromGoesAtOnce) {
  EXPECT_EQ(RepaintDelayNs(kFlip, 0, kPeriod, kWindow), 0U) << "no flip yet";
  EXPECT_EQ(RepaintDelayNs(kFlip - kPeriod, kFlip, kPeriod, kWindow), 0U)
      << "a flip stamped a refresh after now";
  EXPECT_EQ(RepaintDelayNs(kFlip + kMs, kFlip, 0, kWindow), 0U) << "no period";
}

TEST(RepaintWindow, OffOrWiderThanARefreshGoesAtOnce) {
  EXPECT_EQ(RepaintDelayNs(kFlip + kMs, kFlip, kPeriod, 0), 0U);
  EXPECT_EQ(RepaintDelayNs(kFlip + kMs, kFlip, kPeriod, kPeriod), 0U);
}

TEST(RepaintWindow, TheEnvironmentSetsItInMicroseconds) {
  constexpr const char* kVar = "IVI_DRM_REPAINT_WINDOW_US";
  unsetenv(kVar);
  EXPECT_EQ(RepaintWindowNsFromEnv(kWindow), kWindow);
  setenv(kVar, "3000", 1);
  EXPECT_EQ(RepaintWindowNsFromEnv(kWindow), 3 * kMs);
  setenv(kVar, "0", 1);
  EXPECT_EQ(RepaintWindowNsFromEnv(kWindow), 0U) << "0 turns it off";
  setenv(kVar, "3ms", 1);
  EXPECT_EQ(RepaintWindowNsFromEnv(kWindow), kWindow) << "not a number";
  unsetenv(kVar);
}

TEST(RepaintWindow, ACommitIsMeantForTheFirstVblankAfterIt) {
  EXPECT_EQ(TargetVblankNs(kFlip + kMs, kFlip, kPeriod), kFlip + kPeriod);
  EXPECT_EQ(TargetVblankNs(kFlip + kPeriod + kMs, kFlip, kPeriod),
            kFlip + 2 * kPeriod);
  EXPECT_EQ(TargetVblankNs(kFlip, 0, kPeriod), 0U) << "no flip yet";
  EXPECT_EQ(TargetVblankNs(kFlip - kPeriod, kFlip, kPeriod), 0U)
      << "a flip stamped a refresh after the commit";
  EXPECT_EQ(TargetVblankNs(kFlip + 9 * kPeriod, kFlip, kPeriod), 0U)
      << "too long since the last flip to count from";
}

TEST(RepaintWindow, AFlipARefreshLateMissedItsVblank) {
  const uint64_t target = kFlip + kPeriod;
  EXPECT_FALSE(MissedVblank(target, target, kPeriod));
  EXPECT_FALSE(MissedVblank(target, target + kMs, kPeriod)) << "jitter";
  EXPECT_TRUE(MissedVblank(target, target + kPeriod, kPeriod));
  EXPECT_FALSE(MissedVblank(0, target + kPeriod, kPeriod)) << "no target";
}

TEST(RepaintWindow, AMissWidensTheWindowUpToHalfARefresh) {
  uint64_t w = kWindow;
  w = AdaptRepaintWindowNs(w, kWindow, kPeriod, /*missed=*/true);
  EXPECT_EQ(w, kWindow + kMs);
  for (int i = 0; i < 10; ++i) {
    w = AdaptRepaintWindowNs(w, kWindow, kPeriod, true);
  }
  EXPECT_EQ(w, kPeriod / 2) << "capped at half a refresh";
}

TEST(RepaintWindow, FlipsOnTimeEaseItBackToTheBase) {
  uint64_t w = kPeriod / 2;
  int frames = 0;
  while (w > kWindow && frames < 10'000) {
    w = AdaptRepaintWindowNs(w, kWindow, kPeriod, /*missed=*/false);
    ++frames;
  }
  EXPECT_EQ(w, kWindow) << "back to the base, not below it";
  // A few seconds at 60 Hz, not a moment and not forever.
  EXPECT_GT(frames, 60);
  EXPECT_LT(frames, 600);
  EXPECT_EQ(AdaptRepaintWindowNs(kWindow, kWindow, kPeriod, false), kWindow);
}

TEST(RepaintWindow, AWindowThatIsOffStaysOff) {
  EXPECT_EQ(AdaptRepaintWindowNs(0, 0, kPeriod, /*missed=*/true), 0U);
}

TEST(RepaintWindow, AdaptingIsOnUnlessTheEnvironmentSaysZero) {
  constexpr const char* kVar = "IVI_DRM_REPAINT_WINDOW_ADAPT";
  unsetenv(kVar);
  EXPECT_TRUE(RepaintWindowAdaptsFromEnv());
  setenv(kVar, "0", 1);
  EXPECT_FALSE(RepaintWindowAdaptsFromEnv());
  setenv(kVar, "1", 1);
  EXPECT_TRUE(RepaintWindowAdaptsFromEnv());
  unsetenv(kVar);
}

TEST(RepaintWindow, OnlyAPresentThatWaitedForItsWindowIsJudged) {
  // Arrived 1 ms after the flip: it waits for the window, and is judged.
  EXPECT_EQ(JudgedTargetNs(kFlip + kMs, kFlip, kPeriod, kWindow),
            kFlip + kPeriod);
  // Arrived just as the window opened: still on time to be judged.
  EXPECT_EQ(JudgedTargetNs(kFlip + kPeriod - kWindow, kFlip, kPeriod, kWindow),
            kFlip + kPeriod);
  // Arrived inside the window: it commits at once, and a wider window would
  // not have helped it.
  EXPECT_EQ(JudgedTargetNs(kFlip + kPeriod - kMs, kFlip, kPeriod, kWindow), 0U);
  EXPECT_EQ(JudgedTargetNs(kFlip + kMs, kFlip, kPeriod, 0), 0U) << "off";
  EXPECT_EQ(JudgedTargetNs(kFlip + kMs, 0, kPeriod, kWindow), 0U)
      << "no flip yet";
}

TEST(RepaintWindow, AFlipIsJudgedAgainstItsOwnTargetOnly) {
  const uint64_t target = kFlip + kPeriod;
  EXPECT_EQ(JudgeFlip(0, target, kPeriod), FlipVerdict::kUnjudged);
  EXPECT_EQ(JudgeFlip(target, target, kPeriod), FlipVerdict::kOnTime);
  EXPECT_EQ(JudgeFlip(target, target + kPeriod, kPeriod), FlipVerdict::kMissed);
  // A target left behind by a present that never committed is not this
  // flip's.
  EXPECT_EQ(JudgeFlip(target, target + 5 * kPeriod, kPeriod),
            FlipVerdict::kUnjudged);
}
