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

#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "backend/wayland_vulkan/surface_size_latch.h"

namespace {

using wl_vulkan::SurfaceSizeLatch;

TEST(SurfaceSizeLatch, StartsInvalidAndNotStale) {
  SurfaceSizeLatch latch;
  EXPECT_FALSE(latch.Valid());
  EXPECT_FALSE(latch.TakeIfStale().has_value())
      << "nothing recorded yet is not a change to act on";
  EXPECT_EQ(latch.Current(), SurfaceSizeLatch::Size(0, 0));
}

// The reader must get the size exactly once per change. Twice is a redundant
// protocol request; never is a region describing the wrong extent.
TEST(SurfaceSizeLatch, AChangeIsDeliveredExactlyOnce) {
  SurfaceSizeLatch latch;
  EXPECT_TRUE(latch.Set(1280, 720));
  EXPECT_TRUE(latch.Valid());

  const auto first = latch.TakeIfStale();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, SurfaceSizeLatch::Size(1280, 720));

  EXPECT_FALSE(latch.TakeIfStale().has_value()) << "delivered twice";
  // Still readable without the flag, which is how the ring's own claim reads it.
  EXPECT_EQ(latch.Current(), SurfaceSizeLatch::Size(1280, 720));
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

  // And a real move after a no-op still registers.
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
  latch.ClearStale();

  EXPECT_TRUE(latch.Set(1280, 800));
  latch.ClearStale();
  EXPECT_TRUE(latch.Set(1600, 800));
  latch.ClearStale();
  // The rotation case: both move, swapping.
  EXPECT_TRUE(latch.Set(800, 1600));
  EXPECT_EQ(latch.Current(), SurfaceSizeLatch::Size(800, 1600));
}

// Coalescing: several changes before the reader looks collapse to the newest.
// An intermediate extent is of no interest -- only what the surface is now.
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

// ClearStale is how the ring's own claim avoids a duplicate: it applies the
// current size itself, so the reader must not then apply it again. This is the
// defect found in review on #800.
TEST(SurfaceSizeLatch, ClearStaleSuppressesTheDuplicateTake) {
  SurfaceSizeLatch latch;
  ASSERT_TRUE(latch.Set(1280, 720));

  // The ring claims using Current() and consumes the flag itself.
  EXPECT_EQ(latch.Current(), SurfaceSizeLatch::Size(1280, 720));
  latch.ClearStale();

  EXPECT_FALSE(latch.TakeIfStale().has_value())
      << "the present path would have claimed a second time in one commit";

  // A size arriving after the claim still gets its own delivery, which is the
  // case the flag exists for.
  EXPECT_TRUE(latch.Set(1920, 720));
  EXPECT_TRUE(latch.TakeIfStale().has_value());
}

// The ordering guarantee, stated in the header: Set stores then raises, Take
// lowers then reads. So a writer interleaved with a reader may cost a
// redundant delivery but can never leave the latch holding a size the reader
// has not been offered. Losing an update would mean a region describing the
// wrong extent for the life of the surface.
TEST(SurfaceSizeLatch, ConcurrentWritesAreNeverLost) {
  SurfaceSizeLatch latch;
  constexpr int32_t kIterations = 20000;
  std::atomic<bool> go{false};

  std::thread writer([&] {
    while (!go.load(std::memory_order_acquire)) {
    }
    for (int32_t i = 1; i <= kIterations; ++i) {
      latch.Set(i, i + 1);
    }
  });

  SurfaceSizeLatch::Size last{0, 0};
  std::thread reader([&] {
    while (!go.load(std::memory_order_acquire)) {
    }
    for (int32_t i = 0; i < kIterations * 4; ++i) {
      if (const auto s = latch.TakeIfStale()) {
        // Monotonic: a take never goes backwards, which is what proves no
        // reader saw a half-updated pair.
        EXPECT_GE(s->first, last.first);
        EXPECT_EQ(s->second, s->first + 1) << "torn pair";
        last = *s;
      }
    }
  });

  go.store(true, std::memory_order_release);
  writer.join();
  reader.join();

  // Whatever the interleaving, the final size is the writer's last, and it is
  // readable. A lost update would leave Current() behind.
  EXPECT_EQ(latch.Current(),
            SurfaceSizeLatch::Size(kIterations, kIterations + 1));
}

}  // namespace
