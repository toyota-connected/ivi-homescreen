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

#include <cstdint>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "view/layer_scanout.h"

namespace {

constexpr BufferTransform kAll[] = {
    BufferTransform::kNormal,     BufferTransform::k90,
    BufferTransform::k180,        BufferTransform::k270,
    BufferTransform::kFlipped,    BufferTransform::kFlipped90,
    BufferTransform::kFlipped180, BufferTransform::kFlipped270,
};

constexpr uint32_t Fixed(const uint32_t px) {
  return px << 16U;
}

// What a producer does to a w x h image to fill its buffer for `t`, from the
// transform's definition: rotate counter-clockwise by 90-degree steps,
// mirroring around the vertical axis first for the FLIPPED variants. Maps an
// image pixel to its buffer pixel.
std::pair<int, int> ProducerPlaces(BufferTransform t,
                                   int x,
                                   int y,
                                   int w,
                                   int h) {
  if (static_cast<uint32_t>(t) >= 4) {
    x = w - 1 - x;
  }
  switch (static_cast<uint32_t>(t) % 4) {
    case 0:
      return {x, y};
    case 1:  // 90 counter-clockwise: the right column becomes the top row
      return {y, w - 1 - x};
    case 2:
      return {w - 1 - x, h - 1 - y};
    default:  // 270 counter-clockwise
      return {h - 1 - y, x};
  }
}

// What a KMS plane does to a bw x bh buffer for a rotation property, written
// from the kernel's drm_rect_rotate(): reflect first, then rotate
// counter-clockwise. Maps a buffer pixel to the pixel it is shown at.
std::pair<int, int> PlaneShows(uint32_t rotation,
                               int x,
                               int y,
                               int bw,
                               int bh) {
  if ((rotation & kDrmReflectX) != 0) {
    x = bw - 1 - x;
  }
  if ((rotation & kDrmRotate90) != 0) {
    return {y, bw - 1 - x};
  }
  if ((rotation & kDrmRotate180) != 0) {
    return {bw - 1 - x, bh - 1 - y};
  }
  if ((rotation & kDrmRotate270) != 0) {
    return {bh - 1 - y, x};
  }
  return {x, y};
}

}  // namespace

// The rotation handed to the plane undoes what the producer did: every pixel
// of the image lands where it started. Checked on a non-square image so a
// swapped width and height cannot pass by symmetry.
TEST(LayerScanout, PlaneRotationUndoesTheProducersTransform) {
  constexpr int w = 5;
  constexpr int h = 3;
  for (const BufferTransform t : kAll) {
    const bool swaps = TransformSwapsAxes(t);
    const int bw = swaps ? h : w;
    const int bh = swaps ? w : h;
    const uint32_t rotation = DrmRotationFor(t);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const auto [bx, by] = ProducerPlaces(t, x, y, w, h);
        ASSERT_TRUE(bx >= 0 && bx < bw && by >= 0 && by < bh);
        const auto [sx, sy] = PlaneShows(rotation, bx, by, bw, bh);
        EXPECT_EQ(sx, x) << "transform " << static_cast<uint32_t>(t);
        EXPECT_EQ(sy, y) << "transform " << static_cast<uint32_t>(t);
      }
    }
  }
}

// Exactly one rotation bit per value, which the kernel requires.
TEST(LayerScanout, OneRotationBitEach) {
  constexpr uint32_t kRotateMask =
      kDrmRotate0 | kDrmRotate90 | kDrmRotate180 | kDrmRotate270;
  for (const BufferTransform t : kAll) {
    const uint32_t r = DrmRotationFor(t) & kRotateMask;
    EXPECT_NE(r, 0U);
    EXPECT_EQ(r & (r - 1), 0U) << "transform " << static_cast<uint32_t>(t);
  }
}

TEST(LayerScanout, WholeLayerFillsTheView) {
  LayerPlane p;
  ASSERT_TRUE(PlaceLayerOnPlane({}, {}, BufferTransform::kNormal, 64, 32,
                                {100, 50, 64, 32}, &p));
  EXPECT_EQ(p.src.x, 0U);
  EXPECT_EQ(p.src.y, 0U);
  EXPECT_EQ(p.src.w, Fixed(64));
  EXPECT_EQ(p.src.h, Fixed(32));
  EXPECT_EQ(p.dst.x, 100);
  EXPECT_EQ(p.dst.y, 50);
  EXPECT_EQ(p.dst.w, 64);
  EXPECT_EQ(p.dst.h, 32);
  EXPECT_EQ(p.rotation, kDrmRotate0);
}

// A layer placed within the view lands at the view's origin plus its own, and
// keeps its crop at sub-pixel precision.
TEST(LayerScanout, PlacedLayerKeepsItsFractionalCrop) {
  LayerPlane p;
  ASSERT_TRUE(PlaceLayerOnPlane({1.5, 2.25, 16, 8}, {10, 20, 32, 16},
                                BufferTransform::kNormal, 64, 64,
                                {100, 50, 200, 100}, &p));
  EXPECT_EQ(p.src.x, Fixed(1) + 0x8000U);
  EXPECT_EQ(p.src.y, Fixed(2) + 0x4000U);
  EXPECT_EQ(p.src.w, Fixed(16));
  EXPECT_EQ(p.src.h, Fixed(8));
  EXPECT_EQ(p.dst.x, 110);
  EXPECT_EQ(p.dst.y, 70);
  EXPECT_EQ(p.dst.w, 32);
  EXPECT_EQ(p.dst.h, 16);
}

// A layer hanging off the view's right edge is cut at it, and so is its crop
// -- by the same fraction, in the buffer's own orientation.
TEST(LayerScanout, OverhangIsClippedWithItsCrop) {
  LayerPlane p;
  // Half of a 40-wide destination is outside a 60-wide view.
  ASSERT_TRUE(PlaceLayerOnPlane({}, {40, 0, 40, 20}, BufferTransform::kNormal,
                                80, 40, {0, 0, 60, 20}, &p));
  EXPECT_EQ(p.dst.x, 40);
  EXPECT_EQ(p.dst.w, 20);
  EXPECT_EQ(p.src.x, 0U);
  EXPECT_EQ(p.src.w, Fixed(40));
  EXPECT_EQ(p.src.h, Fixed(40));

  // At 90 degrees the destination's right half comes from the buffer's
  // bottom half.
  ASSERT_TRUE(PlaceLayerOnPlane({}, {40, 0, 40, 20}, BufferTransform::k90, 40,
                                80, {0, 0, 60, 20}, &p));
  EXPECT_EQ(p.src.x, 0U);
  EXPECT_EQ(p.src.y, Fixed(40));
  EXPECT_EQ(p.src.w, Fixed(40));
  EXPECT_EQ(p.src.h, Fixed(40));
  EXPECT_EQ(p.rotation, kDrmRotate270);
}

TEST(LayerScanout, InvisibleLayerIsNotPlaced) {
  LayerPlane p;
  EXPECT_FALSE(PlaceLayerOnPlane({}, {70, 0, 10, 10}, BufferTransform::kNormal,
                                 10, 10, {0, 0, 60, 20}, &p));
  EXPECT_FALSE(PlaceLayerOnPlane({}, {}, BufferTransform::kNormal, 0, 10,
                                 {0, 0, 60, 20}, &p));
}

// A crop past the buffer's edge is pulled back inside it: the kernel rejects
// such a plane, and one bad layer would take the frame to the composite path.
TEST(LayerScanout, CropIsKeptInsideTheBuffer) {
  LayerPlane p;
  ASSERT_TRUE(PlaceLayerOnPlane({48, -4, 32, 20}, {0, 0, 32, 16},
                                BufferTransform::kNormal, 64, 64,
                                {0, 0, 32, 16}, &p));
  EXPECT_EQ(p.src.x, Fixed(48));
  EXPECT_EQ(p.src.y, 0U);
  EXPECT_EQ(p.src.w, Fixed(16));
  EXPECT_EQ(p.src.h, Fixed(16));
}

TEST(LayerScanout, OpaqueFormatsDropTheirAlpha) {
  constexpr auto code = [](char a, char b, char c, char d) {
    return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
  };
  EXPECT_EQ(OpaqueScanoutFourcc(code('A', 'R', '2', '4')),
            code('X', 'R', '2', '4'));
  EXPECT_EQ(OpaqueScanoutFourcc(code('A', 'B', '2', '4')),
            code('X', 'B', '2', '4'));
  // Already opaque, or with no alpha-less twin: unchanged.
  EXPECT_EQ(OpaqueScanoutFourcc(code('X', 'R', '2', '4')),
            code('X', 'R', '2', '4'));
  EXPECT_EQ(OpaqueScanoutFourcc(code('N', 'V', '1', '2')),
            code('N', 'V', '1', '2'));
}

TEST(LayerScanout, TagsAreStablePerViewAndLayer) {
  PvLayerTags tags;
  int a = 0;
  int b = 0;
  void* a1 = tags.Get(&a, 1);
  void* a2 = tags.Get(&a, 2);
  void* b1 = tags.Get(&b, 1);
  EXPECT_NE(a1, a2);
  EXPECT_NE(a1, b1);
  EXPECT_EQ(tags.Get(&a, 1), a1);

  const std::vector<void*> of_a = tags.TagsOf(&a);
  ASSERT_EQ(of_a.size(), 2U);
  EXPECT_EQ(tags.TagsOf(&b).size(), 1U);

  tags.RetainOnly(std::vector<void*>{a2});
  EXPECT_EQ(tags.size(), 1U);
  EXPECT_EQ(tags.Get(&a, 2), a2);
  EXPECT_TRUE(tags.TagsOf(&b).empty());
}
