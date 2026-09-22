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

#include <algorithm>
#include <cstdint>
#include <vector>

#include "gtest/gtest.h"

#include "deferred_retire_set.h"

namespace {

// On screen: exactly the ids given.
auto Showing(const std::vector<uint32_t>& ids) {
  return [ids](uint32_t id) {
    return std::any_of(ids.begin(), ids.end(),
                       [id](uint32_t shown) { return shown == id; });
  };
}

}  // namespace

TEST(DeferredRetireSet, OffScreenIdIsDroppedAtOnce) {
  DeferredRetireSet s;
  EXPECT_TRUE(s.Retire(3, /*on_screen=*/false));
  EXPECT_EQ(s.size(), 0u);
  EXPECT_TRUE(s.ReleaseOffScreen(Showing({4})).empty());
}

TEST(DeferredRetireSet, OnScreenIdWaitsForTheNextFrame) {
  DeferredRetireSet s;
  EXPECT_FALSE(s.Retire(3, /*on_screen=*/true));
  EXPECT_TRUE(s.Contains(3));

  // Still on screen: stays deferred.
  EXPECT_TRUE(s.ReleaseOffScreen(Showing({3})).empty());
  EXPECT_TRUE(s.Contains(3));

  EXPECT_EQ(s.ReleaseOffScreen(Showing({4})), std::vector<uint32_t>{3});
  EXPECT_EQ(s.size(), 0u);
}

TEST(DeferredRetireSet, SupersedingFrameThatIsItselfRetiredStaysDeferred) {
  // A pending frame and the current one both retired: when the pending frame
  // becomes current it releases the old one and waits for its own successor.
  DeferredRetireSet s;
  EXPECT_FALSE(s.Retire(1, /*on_screen=*/true));
  EXPECT_FALSE(s.Retire(2, /*on_screen=*/true));
  EXPECT_EQ(s.ReleaseOffScreen(Showing({2})), std::vector<uint32_t>{1});
  EXPECT_TRUE(s.Contains(2));
  EXPECT_EQ(s.ReleaseOffScreen(Showing({5})), std::vector<uint32_t>{2});
}

TEST(DeferredRetireSet, ResubmittedIdMustBeReimported) {
  DeferredRetireSet s;
  EXPECT_FALSE(s.Retire(7, /*on_screen=*/true));
  EXPECT_TRUE(s.Resubmitted(7));
  EXPECT_FALSE(s.Contains(7));  // live again; nothing left to release
  EXPECT_TRUE(s.ReleaseOffScreen(Showing({8})).empty());
  // An id that was never retired is not stale.
  EXPECT_FALSE(s.Resubmitted(9));
}

// Two frames on screen at once (a stashed frame and the current one, on the
// EGL path): neither is released until it leaves.
TEST(DeferredRetireSet, KeepsEveryFrameStillOnScreen) {
  DeferredRetireSet s;
  EXPECT_FALSE(s.Retire(1, /*on_screen=*/true));
  EXPECT_FALSE(s.Retire(2, /*on_screen=*/true));
  EXPECT_TRUE(s.ReleaseOffScreen(Showing({1, 2})).empty());
  EXPECT_EQ(s.ReleaseOffScreen(Showing({2, 3})), std::vector<uint32_t>{1});
  EXPECT_EQ(s.ReleaseOffScreen(Showing({3, 4})), std::vector<uint32_t>{2});
  EXPECT_EQ(s.size(), 0u);
}

TEST(DeferredRetireSet, RetiringTwiceIsHarmless) {
  DeferredRetireSet s;
  EXPECT_FALSE(s.Retire(2, /*on_screen=*/true));
  EXPECT_FALSE(s.Retire(2, /*on_screen=*/true));
  EXPECT_EQ(s.size(), 1u);
  // Retired again once off screen: drop now, and forget the deferral.
  EXPECT_TRUE(s.Retire(2, /*on_screen=*/false));
  EXPECT_EQ(s.size(), 0u);
}
