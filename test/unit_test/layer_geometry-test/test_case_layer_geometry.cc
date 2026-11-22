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
#include <array>
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

// A bottom-up buffer holds the same image with its rows in the other order.
// Drawn through the flipped affine it comes back the right way up, for every
// transform and through a crop: the flip is of memory order only, so the crop
// still names the displayed image.
TEST(LayerGeometry, FlipRowsReadsABottomUpBufferTheRightWayUp) {
  constexpr int kW = 6;
  constexpr int kH = 4;
  const RectF crop{1, 1, 4, 2};
  for (const BufferTransform t : kAll) {
    SCOPED_TRACE(static_cast<uint32_t>(t));
    const UvAffine top = UvForLayer(crop, kW, kH, t);
    const UvAffine bottom = FlipRows(top);
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x) {
        const double s = (x + 0.5) / kW;
        const double tt = (y + 0.5) / kH;
        EXPECT_FLOAT_EQ(bottom.U(s, tt), top.U(s, tt));
        EXPECT_NEAR(bottom.V(s, tt), 1.0F - top.V(s, tt), 1e-6F)
            << "at destination (" << x << ", " << y << ")";
      }
    }
    const UvAffine twice = FlipRows(bottom);
    EXPECT_FLOAT_EQ(twice.b, top.b);
    EXPECT_FLOAT_EQ(twice.d, top.d);
    EXPECT_FLOAT_EQ(twice.ty, top.ty);
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

TEST(LayerGeometry, PlaceCropsToTheClipAndTheSourceWithIt) {
  // A 400 x 200 view at (100, 50), clipped to its top half and right quarter.
  const RectI clip{400, 0, 1000, 150};
  LayerPlacement p;
  ASSERT_TRUE(PlaceLayer({}, {}, BufferTransform::kNormal, false, 800, 400,
                         {100, 50, 400, 200}, &p, &clip));
  EXPECT_EQ(p.dst.x, 400);
  EXPECT_EQ(p.dst.y, 50);
  EXPECT_EQ(p.dst.w, 100);
  EXPECT_EQ(p.dst.h, 100);
  // The right quarter and top half of the buffer.
  EXPECT_FLOAT_EQ(p.uv.a, 0.25f);
  EXPECT_FLOAT_EQ(p.uv.tx, 0.75f);
  EXPECT_FLOAT_EQ(p.uv.d, 0.5f);
  EXPECT_FLOAT_EQ(p.uv.ty, 0.0f);
}

TEST(LayerGeometry, PlaceDropsALayerClippedAway) {
  const RectI clip{0, 0, 50, 50};
  LayerPlacement p;
  EXPECT_FALSE(PlaceLayer({}, {}, BufferTransform::kNormal, false, 10, 10,
                          {100, 100, 10, 10}, &p, &clip));
  const RectI empty{0, 0, 0, 0};
  EXPECT_FALSE(PlaceLayer({}, {}, BufferTransform::kNormal, false, 10, 10,
                          {0, 0, 10, 10}, &p, &empty));
}

TEST(LayerGeometry, ViewBoundsIsTheClipInViewPixels) {
  const RectI view{100, 50, 400, 200};
  const RectI whole = ViewBounds(view, nullptr);
  EXPECT_EQ(whole.x, 0);
  EXPECT_EQ(whole.w, 400);
  const RectI clip{300, 0, 1000, 100};
  const RectI b = ViewBounds(view, &clip);
  EXPECT_EQ(b.x, 200);
  EXPECT_EQ(b.y, 0);
  EXPECT_EQ(b.w, 200);
  EXPECT_EQ(b.h, 50);
}

TEST(LayerGeometry, SubUvKeepsEachPixelsTextureCoordinates) {
  UvAffine uv;
  uv.a = 0.0f;
  uv.b = 0.5f;
  uv.c = -0.25f;
  uv.d = 0.0f;
  uv.tx = 0.75f;
  uv.ty = 0.125f;
  const RectI dst{10, 20, 200, 100};
  const RectI sub{60, 70, 40, 30};
  const UvAffine s = SubUv(uv, dst, sub);
  for (const auto& [px, py] : {std::pair{60.0, 70.0}, std::pair{100.0, 100.0},
                               std::pair{77.5, 81.25}}) {
    const double s_dst = (px - dst.x) / dst.w;
    const double t_dst = (py - dst.y) / dst.h;
    const double s_sub = (px - sub.x) / sub.w;
    const double t_sub = (py - sub.y) / sub.h;
    EXPECT_NEAR(s.U(s_sub, t_sub), uv.U(s_dst, t_dst), 1e-6);
    EXPECT_NEAR(s.V(s_sub, t_sub), uv.V(s_dst, t_dst), 1e-6);
  }
}

namespace {

RoundedRect Rounded(const RectF& rect, const float rx, const float ry) {
  RoundedRect r;
  r.rect = rect;
  for (auto& c : r.radii) {
    c = {rx, ry};
  }
  return r;
}

// Every pixel of dst is in exactly one piece, and only pieces that touch a
// corner box are marked as corners.
void ExpectExactCover(const RectI& dst,
                      const std::array<RoundedPiece, 7>& pieces,
                      const size_t n) {
  std::vector<int> hits(static_cast<size_t>(dst.w) * static_cast<size_t>(dst.h),
                        0);
  for (size_t k = 0; k < n; ++k) {
    const RectI& p = pieces[k].rect;
    ASSERT_GT(p.w, 0);
    ASSERT_GT(p.h, 0);
    ASSERT_GE(p.x, dst.x);
    ASSERT_GE(p.y, dst.y);
    ASSERT_LE(p.x + p.w, dst.x + dst.w);
    ASSERT_LE(p.y + p.h, dst.y + dst.h);
    for (int y = p.y; y < p.y + p.h; ++y) {
      for (int x = p.x; x < p.x + p.w; ++x) {
        ++hits[static_cast<size_t>(y - dst.y) * static_cast<size_t>(dst.w) +
               static_cast<size_t>(x - dst.x)];
      }
    }
  }
  for (const int h : hits) {
    ASSERT_EQ(h, 1);
  }
}

}  // namespace

TEST(LayerGeometry, SplitPutsOnlyTheCornerBoxesUnderTheMask) {
  const RectI dst{0, 0, 1280, 1440};
  const RoundedRect clip = Rounded({0, 0, 1280, 1440}, 120.0f, 120.0f);
  std::array<RoundedPiece, 7> pieces;
  const size_t n = SplitRoundedCorners(dst, clip, pieces);
  ASSERT_EQ(n, 7u);
  ExpectExactCover(dst, pieces, n);
  int masked = 0;
  for (size_t k = 0; k < n; ++k) {
    if (pieces[k].corner) {
      EXPECT_EQ(pieces[k].rect.w, 120);
      EXPECT_EQ(pieces[k].rect.h, 120);
      masked += pieces[k].rect.w * pieces[k].rect.h;
    }
  }
  EXPECT_EQ(masked, 4 * 120 * 120);
}

TEST(LayerGeometry, SplitRoundsTheCornerBoxesOut) {
  // A fractional clip: the box reaches every pixel the curve touches.
  const RectI dst{10, 10, 100, 100};
  const RoundedRect clip = Rounded({10.25, 10.25, 99.5, 99.5}, 20.5f, 10.5f);
  std::array<RoundedPiece, 7> pieces;
  const size_t n = SplitRoundedCorners(dst, clip, pieces);
  ExpectExactCover(dst, pieces, n);
  ASSERT_TRUE(pieces[0].corner);
  EXPECT_EQ(pieces[0].rect.x, 10);
  EXPECT_EQ(pieces[0].rect.w, 21);  // ceil(10.25 + 20.5) - 10
  EXPECT_EQ(pieces[0].rect.h, 11);  // ceil(10.25 + 10.5) - 10
}

TEST(LayerGeometry, SplitGivesTheCornersTheRowWhenTheyMeet) {
  // Corners wider than half the rect: no plain column between them.
  const RectI dst{0, 0, 100, 400};
  const RoundedRect clip = Rounded({0, 0, 100, 400}, 70.0f, 30.0f);
  std::array<RoundedPiece, 7> pieces;
  const size_t n = SplitRoundedCorners(dst, clip, pieces);
  ExpectExactCover(dst, pieces, n);
  ASSERT_EQ(n, 5u);
  for (size_t k = 0; k < n; ++k) {
    EXPECT_EQ(pieces[k].corner, pieces[k].rect.h == 30) << k;
  }
}

TEST(LayerGeometry, SplitOfALayerInsideTheMiddleNeedsNoMask) {
  // A layer that the corners do not reach is one plain piece.
  const RectI dst{200, 200, 300, 300};
  const RoundedRect clip = Rounded({0, 0, 1000, 1000}, 50.0f, 50.0f);
  std::array<RoundedPiece, 7> pieces;
  const size_t n = SplitRoundedCorners(dst, clip, pieces);
  ASSERT_EQ(n, 1u);
  EXPECT_FALSE(pieces[0].corner);
  EXPECT_EQ(pieces[0].rect.w, 300);
}

TEST(LayerGeometry, SubUvOfAnEmptyRectIsTheSame) {
  UvAffine uv;
  uv.a = 0.5f;
  uv.tx = 0.25f;
  const UvAffine s = SubUv(uv, {0, 0, 0, 10}, {0, 0, 0, 10});
  EXPECT_EQ(s.a, uv.a);
  EXPECT_EQ(s.tx, uv.tx);
  EXPECT_FALSE(std::isnan(s.U(0.5, 0.5)));
}

TEST(LayerGeometry, PlaceThroughTheIdentityIsTheRect) {
  QuadPlacement q;
  ASSERT_TRUE(PlaceLayerThrough({}, {}, BufferTransform::kNormal, true, 64, 32,
                                64, 32, ViewMap{}, &q));
  EXPECT_DOUBLE_EQ(q.At(1, 1)[0], 64);
  EXPECT_DOUBLE_EQ(q.At(1, 1)[1], 32);
  // Each edge is the view's, smoothed and grown by a pixel, the bounds with it.
  EXPECT_DOUBLE_EQ(q.grow[0], 1.0 / 64);
  EXPECT_DOUBLE_EQ(q.grow[1], 1.0 / 64);
  EXPECT_DOUBLE_EQ(q.grow[2], 1.0 / 32);
  EXPECT_DOUBLE_EQ(q.grow[3], 1.0 / 32);
  EXPECT_EQ(q.bounds.x, -1);
  EXPECT_EQ(q.bounds.y, -1);
  EXPECT_EQ(q.bounds.w, 66);
  EXPECT_EQ(q.bounds.h, 34);
  EXPECT_TRUE(q.opaque);
}

TEST(LayerGeometry, PlaceThroughATurnTurnsTheQuad) {
  // A quarter turn: (x, y) -> (200 - y, 100 + x).
  ViewMap m;
  m.a = 0;
  m.b = 1;
  m.c = -1;
  m.d = 0;
  m.tx = 200;
  m.ty = 100;
  QuadPlacement q;
  ASSERT_TRUE(PlaceLayerThrough({}, {}, BufferTransform::kNormal, false, 200,
                                100, 200, 100, m, &q));
  EXPECT_DOUBLE_EQ(q.At(0, 0)[0], 200);
  EXPECT_DOUBLE_EQ(q.At(0, 0)[1], 100);
  EXPECT_DOUBLE_EQ(q.At(1, 0)[0], 200);  // s runs down the output
  EXPECT_DOUBLE_EQ(q.At(1, 0)[1], 300);
  EXPECT_DOUBLE_EQ(q.At(0, 1)[0], 100);  // and t to the left
  // A pixel past each edge.
  EXPECT_EQ(q.bounds.x, 99);
  EXPECT_EQ(q.bounds.y, 99);
  EXPECT_EQ(q.bounds.w, 102);
  EXPECT_EQ(q.bounds.h, 202);
  // The texture coordinates still run over the layer, not the bounds.
  EXPECT_FLOAT_EQ(q.uv.U(1, 0), 1);
  EXPECT_FLOAT_EQ(q.uv.V(0, 1), 1);
}

TEST(LayerGeometry, PlaceThroughRoundsTheBoundsOutAndClipsThem) {
  // An eighth of a turn about (50, 50).
  const double k = std::sqrt(0.5);
  ViewMap m;
  m.a = k;
  m.b = k;
  m.c = -k;
  m.d = k;
  m.tx = 50;
  m.ty = 50;
  QuadPlacement q;
  ASSERT_TRUE(PlaceLayerThrough({}, {}, BufferTransform::kNormal, false, 10, 10,
                                10, 10, m, &q));
  // Corners at x 50 - 7.07 .. 50 + 7.07, y 50 .. 50 + 14.14, each moved
  // out along its diagonal by sqrt(2) as both its edges move out by 1.
  EXPECT_EQ(q.bounds.x, 41);
  EXPECT_EQ(q.bounds.y, 48);
  EXPECT_EQ(q.bounds.x + q.bounds.w, 59);
  EXPECT_EQ(q.bounds.y + q.bounds.h, 66);
  const RectI clip{0, 0, 100, 55};
  ASSERT_TRUE(PlaceLayerThrough({}, {}, BufferTransform::kNormal, false, 10, 10,
                                10, 10, m, &q, &clip));
  EXPECT_EQ(q.bounds.y + q.bounds.h, 55);
  const RectI away{0, 0, 10, 10};
  EXPECT_FALSE(PlaceLayerThrough({}, {}, BufferTransform::kNormal, false, 10,
                                 10, 10, 10, m, &q, &away));
}

TEST(LayerGeometry, PlaceThroughCropsTheLayerToTheViewFirst) {
  // A layer hanging off the right of a 100x100 view: only its left half is
  // drawn, from the left half of its buffer.
  QuadPlacement q;
  ASSERT_TRUE(PlaceLayerThrough({}, {50, 0, 100, 100}, BufferTransform::kNormal,
                                false, 100, 100, 100, 100, ViewMap{}, &q));
  EXPECT_DOUBLE_EQ(q.origin[0], 50);
  EXPECT_DOUBLE_EQ(q.At(1, 1)[0], 100);
  EXPECT_FLOAT_EQ(q.uv.U(1, 0), 0.5f);
}

TEST(LayerGeometry, PlaceThroughAPerspectiveProjectsTheCorners) {
  // w = 1 + x / 100 on a 100x50 view: the right edge at half its distance.
  ViewMap m;
  m.g = 0.01;
  QuadPlacement q;
  ASSERT_TRUE(PlaceLayerThrough({}, {}, BufferTransform::kNormal, false, 100,
                                50, 100, 50, m, &q));
  EXPECT_DOUBLE_EQ(q.W(0, 0), 1);
  EXPECT_DOUBLE_EQ(q.W(1, 0), 2);
  EXPECT_DOUBLE_EQ(q.At(1, 0)[0], 50);
  EXPECT_DOUBLE_EQ(q.At(1, 1)[1], 25);
  EXPECT_DOUBLE_EQ(q.At(0, 1)[1], 50);
  // Grown past every edge: by a pixel or more, more at an edge's near end,
  // where a unit of s or t covers more of the output than at its far one.
  EXPECT_LE(q.bounds.x, -1);
  EXPECT_LE(q.bounds.y, -1);
  EXPECT_GE(q.bounds.x + q.bounds.w, 51);
  EXPECT_GE(q.bounds.y + q.bounds.h, 51);
  // The texture coordinates still run over the layer.
  EXPECT_FLOAT_EQ(q.uv.U(1, 0), 1);
}

// An eighth of a turn about (50, 50) of a 10x10 view: a diamond.
ViewMap EighthTurn() {
  const double k = std::sqrt(0.5);
  ViewMap m;
  m.a = k;
  m.b = k;
  m.c = -k;
  m.d = k;
  m.tx = 50;
  m.ty = 50;
  return m;
}

TEST(LayerGeometry, EdgeDistancesArePixelsToEachEdgeAboveZeroInside) {
  QuadPlacement q;
  ASSERT_TRUE(PlaceLayerThrough({}, {}, BufferTransform::kNormal, false, 10, 10,
                                10, 10, EighthTurn(), &q));
  // The quad's center is 5 pixels from each edge.
  const auto mid = q.At(0.5, 0.5);
  for (const double e : EdgeDistances(q, mid[0], mid[1])) {
    EXPECT_NEAR(e, 5.0, 1e-9);
  }
  // The corner at (0, 0) is on the edges s = 0 and t = 0, and 10 pixels
  // inside the other two.
  const auto c = q.At(0, 0);
  const auto e = EdgeDistances(q, c[0], c[1]);
  EXPECT_NEAR(e[0], 0.0, 1e-9);
  EXPECT_NEAR(e[1], 10.0, 1e-9);
  EXPECT_NEAR(e[2], 0.0, 1e-9);
  EXPECT_NEAR(e[3], 10.0, 1e-9);
  // Above the top corner, outside two edges.
  const auto out = EdgeDistances(q, 50, 48);
  EXPECT_LT(std::min(out[0], out[2]), 0.0);
}

TEST(LayerGeometry, EdgeDistancesKeepTheirSignUnderAMirror) {
  ViewMap m;
  m.a = -1;
  m.tx = 10;
  QuadPlacement q;
  ASSERT_TRUE(PlaceLayerThrough({}, {}, BufferTransform::kNormal, false, 10, 10,
                                10, 10, m, &q));
  for (const double e : EdgeDistances(q, 3, 4)) {
    EXPECT_GT(e, 0.0);
  }
  EXPECT_LT(EdgeDistances(q, -0.5, 4)[1], 0.0);  // past s = 1, now at x = 0
}

TEST(LayerGeometry, PlaceThroughGrowsEachViewEdgeByAPixel) {
  QuadPlacement q;
  ASSERT_TRUE(PlaceLayerThrough({}, {}, BufferTransform::kNormal, false, 10, 10,
                                10, 10, EighthTurn(), &q));
  ASSERT_TRUE(q.Smoothed());
  // Every grown corner is a pixel outside both edges it is on, and as far
  // inside the other two as the quad is wide plus a pixel.
  for (const auto& [s, t] : q.GrownCorners()) {
    const auto p = q.At(s, t);
    const auto e = EdgeDistances(q, p[0], p[1]);
    EXPECT_NEAR(std::min({e[0], e[1], e[2], e[3]}), -1.0, 1e-9);
    EXPECT_NEAR(std::max({e[0], e[1], e[2], e[3]}), 11.0, 1e-9);
  }
}

TEST(LayerGeometry, PlaceThroughLeavesEdgesInsideTheViewHard) {
  // A layer over the view's left half: its right edge meets the next layer's,
  // which smoothing both would open into a seam.
  QuadPlacement q;
  ASSERT_TRUE(PlaceLayerThrough({}, {0, 0, 5, 10}, BufferTransform::kNormal,
                                false, 10, 10, 10, 10, EighthTurn(), &q));
  EXPECT_GT(q.grow[0], 0.0);
  EXPECT_EQ(q.grow[1], 0.0);
  EXPECT_GT(q.grow[2], 0.0);
  EXPECT_GT(q.grow[3], 0.0);
  // One clear of every edge is not smoothed at all.
  ASSERT_TRUE(PlaceLayerThrough({}, {2, 2, 6, 6}, BufferTransform::kNormal,
                                false, 10, 10, 10, 10, EighthTurn(), &q));
  EXPECT_FALSE(q.Smoothed());
  const RectI unrounded{41, 50, 17, 15};  // as before smoothing, rounded out
  EXPECT_GE(q.bounds.x, unrounded.x);
  EXPECT_LE(q.bounds.x + q.bounds.w, unrounded.x + unrounded.w);
}

TEST(LayerGeometry, PlaceThroughGrowsAPerspectiveByAPixelAtLeast) {
  // w = 1 + x / 100: the right edge is at half its distance, so a unit of s
  // or t is half as many pixels there.
  ViewMap m;
  m.g = 0.01;
  QuadPlacement q;
  ASSERT_TRUE(PlaceLayerThrough({}, {}, BufferTransform::kNormal, false, 100,
                                50, 100, 50, m, &q));
  for (const auto& [s, t] : q.GrownCorners()) {
    EXPECT_GT(q.W(s, t), 0.0);
    const auto p = q.At(s, t);
    const auto e = EdgeDistances(q, p[0], p[1]);
    // Outside both of its edges by about a pixel, well past the half a
    // pixel the smoothing reaches (the grow is worked out from the rate at
    // the edge, which a perspective changes a little over a pixel), and not
    // by many.
    std::array<double, 4> sorted = e;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_LE(sorted[1], -0.9);
    EXPECT_GE(sorted[0], -3.0);
  }
}

TEST(LayerGeometry, GrowStaysInFrontOfTheViewer) {
  // w = 1 - x / 101 on a 100 wide view: w at the right edge is 1/101, close
  // to the horizon, where the view is stretched far across the output.
  ViewMap m;
  m.g = -1.0 / 101;
  QuadPlacement q;
  ASSERT_TRUE(PlaceLayerThrough({}, {}, BufferTransform::kNormal, false, 100,
                                50, 100, 50, m, &q));
  for (const auto& [s, t] : q.GrownCorners()) {
    EXPECT_GT(
        q.W(s, t),
        0.5 * q.W(std::clamp(s, 0.0, 1.0), std::clamp(t, 0.0, 1.0)) - 1e-12);
  }
  // The right edge still moves out by a pixel.
  const auto p = q.At(1.0 + q.grow[1], 0.5);
  EXPECT_NEAR(EdgeDistances(q, p[0], p[1])[1], -1.0, 1e-3);
}

TEST(LayerGeometry, GrowAtAnEdgeWhereWFallsIsHeldToHalfOfIt) {
  // w = 1 + x on a 1 pixel wide view scaled up 100 times, so a unit of s is
  // under a pixel near nothing and growing s = 0 by a pixel would take w
  // from 1 toward 0. Held to half.
  ViewMap m;
  m.a = 0.01;
  m.d = 0.01;
  m.g = 1.0;
  m.h = 0.0;
  QuadPlacement q;
  ASSERT_TRUE(PlaceLayerThrough({}, {}, BufferTransform::kNormal, false, 1, 1,
                                1, 1, m, &q));
  EXPECT_GE(q.W(-q.grow[0], 0), 0.5 - 1e-12);
  for (const auto& [s, t] : q.GrownCorners()) {
    EXPECT_GT(q.W(s, t), 0.0);
  }
}

TEST(LayerGeometry, PlaceThroughDropsALayerPastTheHorizon) {
  ViewMap m;
  m.g = -0.02;  // w = 0 halfway across a 100 wide view
  QuadPlacement q;
  EXPECT_FALSE(PlaceLayerThrough({}, {}, BufferTransform::kNormal, false, 100,
                                 50, 100, 50, m, &q));
}

namespace {

ClipShape RoundedShape(double x, double y, double w, double h, float r) {
  ClipShape c;
  c.shape.rect = {x, y, w, h};
  for (auto& corner : c.shape.radii) {
    corner = {r, r};
  }
  return c;
}

// Checks @p pieces against the coverage of @p shapes (and @p quad's edges)
// at every pixel of @p bounds: each pixel is in at most one piece; one in a
// whole piece is covered whole; one in no piece is not covered at all; one
// partly covered is in a masked piece. Returns the pixels in each kind.
struct PieceCounts {
  int whole{0};
  int masked{0};
  int left_out{0};
};
PieceCounts CheckPieces(const ClipShapes& shapes,
                        const QuadPlacement* quad,
                        const RectI& bounds,
                        const std::vector<ClipPiece>& pieces) {
  PieceCounts n;
  for (int y = bounds.y; y < bounds.y + bounds.h; ++y) {
    for (int x = bounds.x; x < bounds.x + bounds.w; ++x) {
      const ClipPiece* in = nullptr;
      for (const ClipPiece& p : pieces) {
        const RectI& r = p.rect;
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
          EXPECT_EQ(in, nullptr) << "two pieces at " << x << "," << y;
          in = &p;
        }
      }
      double cover = 1.0;
      for (size_t k = 0; k < shapes.count; ++k) {
        cover = std::min(cover,
                         ClipShapeCoverage(shapes.items[k], x + 0.5, y + 0.5));
      }
      if (quad != nullptr) {
        for (const double e : EdgeDistances(*quad, x + 0.5, y + 0.5)) {
          cover = std::min(cover, std::clamp(e + 0.5, 0.0, 1.0));
        }
      }
      if (in == nullptr) {
        ++n.left_out;
        EXPECT_EQ(cover, 0.0) << "left out at " << x << "," << y;
      } else if (!in->mask) {
        ++n.whole;
        EXPECT_EQ(cover, 1.0) << "drawn whole at " << x << "," << y;
      } else {
        ++n.masked;
      }
    }
  }
  return n;
}

}  // namespace

TEST(LayerGeometry, ClipPiecesMaskOnlyNearTheEdges) {
  // Nested rounded clips on an untransformed view: the mask is worked out in
  // the corners and along the inner clip's sides, and most of the view is
  // drawn whole.
  ClipShapes shapes;
  shapes.count = 2;
  shapes.items[0] = RoundedShape(0, 0, 400, 320, 150);
  shapes.items[1] = RoundedShape(5, 5, 390, 310, 8);
  const RectI bounds{5, 5, 390, 310};
  std::vector<ClipPiece> pieces;
  ClipPieces(shapes, nullptr, bounds, &pieces);
  const PieceCounts n = CheckPieces(shapes, nullptr, bounds, pieces);
  EXPECT_GT(n.whole, n.masked * 2);
  EXPECT_GT(n.left_out, 0) << "the outer clip's corners";
  // Joined into rects, not one per cell.
  EXPECT_LT(pieces.size(), 60U);
}

TEST(LayerGeometry, ClipPiecesFollowATurnedView) {
  // A 100x60 view turned 30 degrees about (100, 100), its clip around it.
  const double r = M_PI / 6;
  ViewMap m;
  m.a = std::cos(r);
  m.b = std::sin(r);
  m.c = -std::sin(r);
  m.d = std::cos(r);
  m.tx = 100 - (m.a * 50 + m.c * 30);
  m.ty = 100 - (m.b * 50 + m.d * 30);
  QuadPlacement q;
  ASSERT_TRUE(PlaceLayerThrough({}, {}, BufferTransform::kNormal, false, 100,
                                60, 100, 60, m, &q));
  ClipShapes shapes;
  shapes.count = 1;
  shapes.items[0] = RoundedShape(0, 0, 100, 60, 12);
  shapes.items[0].to_shape = *InverseMap(m);
  std::vector<ClipPiece> pieces;
  ClipPieces(shapes, &q, q.bounds, &pieces);
  const PieceCounts n = CheckPieces(shapes, &q, q.bounds, pieces);
  EXPECT_GT(n.whole, 0);
  EXPECT_GT(n.left_out, 0) << "the corners of the bounds";
}

TEST(LayerGeometry, ClipPiecesUnderAPerspectiveLeaveNothingOut) {
  // A shape under a perspective has no distance to trust far from it, so
  // nothing is left out for it; what is left out is outside the quad.
  ViewMap m;
  m.g = 0.004;
  QuadPlacement q;
  ASSERT_TRUE(PlaceLayerThrough({}, {}, BufferTransform::kNormal, false, 100,
                                80, 100, 80, m, &q));
  ClipShapes shapes;
  shapes.count = 1;
  shapes.items[0] = RoundedShape(0, 0, 100, 80, 20);
  shapes.items[0].to_shape = *InverseMap(m);
  std::vector<ClipPiece> pieces;
  ClipPieces(shapes, &q, q.bounds, &pieces);
  const PieceCounts n = CheckPieces(shapes, &q, q.bounds, pieces);
  EXPECT_GT(n.whole, 0);
}

TEST(LayerGeometry, TheCacheFollowsItsInputs) {
  ClipShapes shapes;
  shapes.count = 1;
  shapes.items[0] = RoundedShape(0, 0, 200, 100, 40);
  ClipPiecesCache cache;
  std::vector<ClipPiece> direct;
  ClipPieces(shapes, nullptr, {0, 0, 200, 100}, &direct);
  ASSERT_EQ(cache.Get(shapes, nullptr, {0, 0, 200, 100}).size(), direct.size());
  EXPECT_EQ(cache.Get(shapes, nullptr, {0, 0, 200, 100}).size(), direct.size());
  // Moved: cut again, to match.
  shapes.items[0].to_shape.tx = -10;
  ClipPieces(shapes, nullptr, {10, 0, 200, 100}, &direct);
  const auto& moved = cache.Get(shapes, nullptr, {10, 0, 200, 100});
  ASSERT_EQ(moved.size(), direct.size());
  for (size_t k = 0; k < moved.size(); ++k) {
    EXPECT_EQ(moved[k].rect.x, direct[k].rect.x);
    EXPECT_EQ(moved[k].mask, direct[k].mask);
  }
}
