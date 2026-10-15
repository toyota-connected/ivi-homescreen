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

// The decision sequence behind wl_surface.set_opaque_region: a size learned on
// the Wayland event thread, consumed on the raster thread, which is the only
// thread allowed to touch the surface. Two defects shipped here before it was
// extracted -- a region sized in buffer rather than surface coordinates, and a
// declaration issued from the writer's thread -- so what is worth pinning is
// the handoff, not the arithmetic.
//
// There is no concurrency test here on purpose. Every method is one critical
// section, so there is no interleaving left to assert. An earlier version kept
// the flag outside the mutex and tested "no update is lost" with two threads
// and 20000 writes; review measured that a mutant with the ordering reversed
// passed it 5/5, because a lost update needs the writer's final store to land
// inside a nanosecond window. The property is now true by construction, which
// is why it is no longer asserted.

#include <gtest/gtest.h>

#include "backend/wayland_vulkan/surface_size_latch.h"

namespace {

using wl_vulkan::SurfaceSizeLatch;

TEST(SurfaceSizeLatch, StartsWithNothingToDeliver) {
  SurfaceSizeLatch latch;
  EXPECT_FALSE(latch.TakeIfStale().has_value())
      << "nothing recorded yet is not a change to act on";
  // Zero is what a caller sees before the window has mapped the surface, and
  // that is the signal to declare nothing rather than an error.
  EXPECT_EQ(latch.Take(), SurfaceSizeLatch::Size(0, 0));
}

// The reader must get a change exactly once. Twice is a redundant protocol
// request inside one commit; never is a region describing the wrong extent for
// the life of the surface.
TEST(SurfaceSizeLatch, AChangeIsDeliveredExactlyOnce) {
  SurfaceSizeLatch latch;
  EXPECT_TRUE(latch.Set(1280, 720));

  const auto first = latch.TakeIfStale();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, SurfaceSizeLatch::Size(1280, 720));

  EXPECT_FALSE(latch.TakeIfStale().has_value()) << "delivered twice";
}

// An idempotent re-declaration is the common case: the window declares its
// mapping on construction, on configure and on every resize, mostly with the
// same numbers. None of those should cost a protocol request.
TEST(SurfaceSizeLatch, SameSizeIsNotAChange) {
  SurfaceSizeLatch latch;
  ASSERT_TRUE(latch.Set(1920, 720));
  ASSERT_TRUE(latch.TakeIfStale().has_value());

  EXPECT_FALSE(latch.Set(1920, 720));
  EXPECT_FALSE(latch.TakeIfStale().has_value());

  // A real move after a no-op still registers.
  EXPECT_TRUE(latch.Set(1280, 720));
  const auto moved = latch.TakeIfStale();
  ASSERT_TRUE(moved.has_value());
  EXPECT_EQ(*moved, SurfaceSizeLatch::Size(1280, 720));
}

// Only one axis moving is still a move. A width-only comparison would miss a
// rotation, where the two swap.
TEST(SurfaceSizeLatch, EitherAxisAloneIsAChange) {
  SurfaceSizeLatch latch;
  ASSERT_TRUE(latch.Set(1280, 720));
  (void)latch.Take();

  EXPECT_TRUE(latch.Set(1280, 800));
  (void)latch.Take();
  EXPECT_TRUE(latch.Set(1600, 800));
  (void)latch.Take();
  // The rotation case: both move, swapping.
  EXPECT_TRUE(latch.Set(800, 1600));
  EXPECT_EQ(latch.Take(), SurfaceSizeLatch::Size(800, 1600));
}

// Several changes before the reader looks collapse to the newest. An
// intermediate extent is of no interest -- only what the surface is now.
TEST(SurfaceSizeLatch, UnreadChangesCollapseToTheNewest) {
  SurfaceSizeLatch latch;
  ASSERT_TRUE(latch.Set(640, 480));
  ASSERT_TRUE(latch.Set(1280, 720));
  ASSERT_TRUE(latch.Set(1920, 1080));

  const auto taken = latch.TakeIfStale();
  ASSERT_TRUE(taken.has_value());
  EXPECT_EQ(*taken, SurfaceSizeLatch::Size(1920, 1080));
  EXPECT_FALSE(latch.TakeIfStale().has_value());
}

// Take is how the ring's own claim avoids a duplicate: it applies the size
// itself, so the present path must not apply it again in the same commit.
// This is the defect found in review on #800.
TEST(SurfaceSizeLatch, TakeSuppressesTheFollowingDelivery) {
  SurfaceSizeLatch latch;
  ASSERT_TRUE(latch.Set(1280, 720));

  EXPECT_EQ(latch.Take(), SurfaceSizeLatch::Size(1280, 720));
  EXPECT_FALSE(latch.TakeIfStale().has_value())
      << "the present path would have claimed a second time in one commit";

  // A size arriving after the claim still gets its own delivery, which is the
  // case the flag exists for.
  EXPECT_TRUE(latch.Set(1920, 720));
  const auto after = latch.TakeIfStale();
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(*after, SurfaceSizeLatch::Size(1920, 720));
}

}  // namespace
