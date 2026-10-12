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

TEST(RepaintWindow, NothingToCountFromGoesAtOnce) {
  EXPECT_EQ(RepaintDelayNs(kFlip, 0, kPeriod, kWindow), 0U) << "no flip yet";
  EXPECT_EQ(RepaintDelayNs(kFlip - kMs, kFlip, kPeriod, kWindow), 0U)
      << "a flip stamped after now";
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
