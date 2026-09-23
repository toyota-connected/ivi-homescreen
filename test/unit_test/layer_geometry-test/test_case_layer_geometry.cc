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

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "layer_geometry.h"

namespace {

// Offset of pixel (x, y) in a row-major array of `n`-component pixels, `w`
// pixels wide. Every operand widened, so no index is formed in int.
size_t Px(int x, int y, int w, int n = 4) {
  return (static_cast<size_t>(y) * static_cast<size_t>(w) +
          static_cast<size_t>(x)) *
         static_cast<size_t>(n);
}

constexpr BufferTransform kAll[] = {
    BufferTransform::kNormal,     BufferTransform::k90,
    BufferTransform::k180,        BufferTransform::k270,
    BufferTransform::kFlipped,    BufferTransform::kFlipped90,
    BufferTransform::kFlipped180, BufferTransform::kFlipped270,
};

// What a producer does to a w x h image to fill its buffer for `t`, written
// from the transform's definition rather than from TransformedCoord: rotate
// counter-clockwise by 90-degree steps, mirroring around the vertical axis
// first for the FLIPPED variants. Maps an image pixel to its buffer pixel.
std::pair<int, int> ProducerPlaces(BufferTransform t,
                                   int x,
                                   int y,
                                   int w,
                                   int h) {
  const bool flipped = static_cast<uint32_t>(t) >= 4;
  if (flipped) {
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

}  // namespace

TEST(LayerGeometry, WholeBufferNormalIsIdentity) {
  const UvAffine m = UvForLayer({}, 64, 32, BufferTransform::kNormal);
  EXPECT_FLOAT_EQ(m.a, 1);
  EXPECT_FLOAT_EQ(m.b, 0);
  EXPECT_FLOAT_EQ(m.c, 0);
  EXPECT_FLOAT_EQ(m.d, 1);
  EXPECT_FLOAT_EQ(m.tx, 0);
  EXPECT_FLOAT_EQ(m.ty, 0);
}

TEST(LayerGeometry, CropScalesAndOffsetsIntoTheBuffer) {
  const UvAffine m =
      UvForLayer({10, 20, 100, 50}, 200, 100, BufferTransform::kNormal);
  EXPECT_FLOAT_EQ(m.tx, 0.05f);
  EXPECT_FLOAT_EQ(m.ty, 0.2f);
  EXPECT_FLOAT_EQ(m.a, 0.5f);
  EXPECT_FLOAT_EQ(m.d, 0.5f);
  EXPECT_FLOAT_EQ(m.U(1, 1), 0.55f);  // the crop's far corner
  EXPECT_FLOAT_EQ(m.V(1, 1), 0.7f);
}

// Draw a producer's transformed buffer back through the affine and recover the
// original image, pixel for pixel, for all eight transforms. A non-square
// image catches a swapped width and height.
TEST(LayerGeometry, EveryTransformUndoesWhatTheProducerDid) {
  constexpr int kW = 5;
  constexpr int kH = 3;
  for (const BufferTransform t : kAll) {
    SCOPED_TRACE(static_cast<uint32_t>(t));
    const bool swap = TransformSwapsAxes(t);
    const int bw = swap ? kH : kW;
    const int bh = swap ? kW : kH;
    std::vector<int> buffer(Px(0, bh, bw, 1), -1);
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x) {
        const auto [bx, by] = ProducerPlaces(t, x, y, kW, kH);
        ASSERT_GE(bx, 0);
        ASSERT_LT(bx, bw);
        ASSERT_GE(by, 0);
        ASSERT_LT(by, bh);
        buffer[Px(bx, by, bw, 1)] = y * kW + x;
      }
    }
    const UvAffine m =
        UvForLayer({}, static_cast<uint32_t>(bw), static_cast<uint32_t>(bh), t);
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x) {
        const double s = (x + 0.5) / kW;
        const double tt = (y + 0.5) / kH;
        const int bx =
            static_cast<int>(std::floor(static_cast<double>(m.U(s, tt)) * bw));
        const int by =
            static_cast<int>(std::floor(static_cast<double>(m.V(s, tt)) * bh));
        ASSERT_GE(bx, 0);
        ASSERT_LT(bx, bw);
        ASSERT_GE(by, 0);
        ASSERT_LT(by, bh);
        EXPECT_EQ(buffer[Px(bx, by, bw, 1)], y * kW + x)
            << "at destination pixel (" << x << ", " << y << ")";
      }
    }
  }
}

TEST(LayerGeometry, AxesSwapExactlyForTheQuarterTurns) {
  EXPECT_FALSE(TransformSwapsAxes(BufferTransform::kNormal));
  EXPECT_TRUE(TransformSwapsAxes(BufferTransform::k90));
  EXPECT_FALSE(TransformSwapsAxes(BufferTransform::k180));
  EXPECT_TRUE(TransformSwapsAxes(BufferTransform::k270));
  EXPECT_FALSE(TransformSwapsAxes(BufferTransform::kFlipped));
  EXPECT_TRUE(TransformSwapsAxes(BufferTransform::kFlipped90));
  EXPECT_FALSE(TransformSwapsAxes(BufferTransform::kFlipped180));
  EXPECT_TRUE(TransformSwapsAxes(BufferTransform::kFlipped270));
}

TEST(LayerGeometry, ClipInsideTheViewChangesNothing) {
  RectF src{};
  RectI dst{10, 10, 100, 50};
  ASSERT_TRUE(ClipLayerToView(&src, &dst, 200, 100, BufferTransform::kNormal,
                              800, 480));
  EXPECT_EQ(dst.x, 10);
  EXPECT_EQ(dst.w, 100);
  EXPECT_DOUBLE_EQ(src.w, 200);  // whole buffer, made explicit
  EXPECT_DOUBLE_EQ(src.h, 100);
}

TEST(LayerGeometry, ClipCropsTheSourceByTheSameFraction) {
  // Half the layer hangs off the left edge; the left half of the crop goes.
  RectF src{0, 0, 200, 100};
  RectI dst{-50, 0, 100, 50};
  ASSERT_TRUE(ClipLayerToView(&src, &dst, 200, 100, BufferTransform::kNormal,
                              800, 480));
  EXPECT_EQ(dst.x, 0);
  EXPECT_EQ(dst.w, 50);
  EXPECT_DOUBLE_EQ(src.x, 100);
  EXPECT_DOUBLE_EQ(src.w, 100);
  EXPECT_DOUBLE_EQ(src.h, 100);
}

// Under a quarter turn the destination's left edge is the buffer's bottom
// edge, so clipping on the left crops rows, not columns.
TEST(LayerGeometry, ClipFollowsTheTransform) {
  RectF src{0, 0, 100, 200};  // buffer is 100 x 200, drawn as 200 x 100
  RectI dst{-100, 0, 200, 100};
  ASSERT_TRUE(
      ClipLayerToView(&src, &dst, 100, 200, BufferTransform::k90, 800, 480));
  EXPECT_EQ(dst.w, 100);
  EXPECT_DOUBLE_EQ(src.x, 0);
  EXPECT_DOUBLE_EQ(src.w, 100);
  EXPECT_DOUBLE_EQ(src.y, 0);  // the top half of the buffer is kept
  EXPECT_DOUBLE_EQ(src.h, 100);
}

TEST(LayerGeometry, LayerOutsideTheViewIsDropped) {
  RectF src{1, 2, 3, 4};
  RectI dst{900, 0, 100, 50};
  EXPECT_FALSE(ClipLayerToView(&src, &dst, 200, 100, BufferTransform::kNormal,
                               800, 480));
  EXPECT_EQ(dst.x, 900);  // untouched
  EXPECT_DOUBLE_EQ(src.x, 1);
  RectI empty{0, 0, 0, 10};
  EXPECT_FALSE(ClipLayerToView(&src, &empty, 200, 100, BufferTransform::kNormal,
                               800, 480));
}

TEST(LayerGeometry, PlaceDefaultsToTheWholeView) {
  LayerPlacement p;
  ASSERT_TRUE(PlaceLayer({}, {}, BufferTransform::kNormal, false, 64, 32,
                         {100, 50, 640, 320}, &p));
  EXPECT_EQ(p.dst.x, 100);
  EXPECT_EQ(p.dst.y, 50);
  EXPECT_EQ(p.dst.w, 640);
  EXPECT_EQ(p.dst.h, 320);
  EXPECT_FLOAT_EQ(p.uv.a, 1);
  EXPECT_FLOAT_EQ(p.uv.d, 1);
}

TEST(LayerGeometry, PlaceOffsetsIntoTheViewAndClipsToIt) {
  // A popup 200 px wide placed at x = 500 in a 600 px wide view: half shows.
  LayerPlacement p;
  ASSERT_TRUE(PlaceLayer({}, {500, 10, 200, 100}, BufferTransform::kNormal,
                         true, 200, 100, {100, 50, 600, 400}, &p));
  EXPECT_EQ(p.dst.x, 600);
  EXPECT_EQ(p.dst.y, 60);
  EXPECT_EQ(p.dst.w, 100);
  EXPECT_EQ(p.dst.h, 100);
  EXPECT_FLOAT_EQ(p.uv.a, 0.5f);  // the left half of the buffer
  EXPECT_FLOAT_EQ(p.uv.tx, 0.0f);
  EXPECT_TRUE(p.opaque);
}

TEST(LayerGeometry, PlaceDropsWhatTheViewCannotShow) {
  LayerPlacement p;
  EXPECT_FALSE(PlaceLayer({}, {700, 0, 50, 50}, BufferTransform::kNormal, false,
                          50, 50, {0, 0, 600, 400}, &p));
  EXPECT_FALSE(PlaceLayer({}, {}, BufferTransform::kNormal, false, 0, 0,
                          {0, 0, 600, 400}, &p));
}
