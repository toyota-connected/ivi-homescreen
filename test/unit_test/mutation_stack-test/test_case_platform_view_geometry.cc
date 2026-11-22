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
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "view/platform_view_geometry.h"

namespace pv = ihs::platform_view;

namespace {

FlutterPlatformViewMutation Transform(double sx,
                                      double kx,
                                      double tx,
                                      double ky,
                                      double sy,
                                      double ty,
                                      double p0 = 0,
                                      double p1 = 0,
                                      double p2 = 1) {
  FlutterPlatformViewMutation m{};
  m.type = kFlutterPlatformViewMutationTypeTransformation;
  m.transformation = {sx, kx, tx, ky, sy, ty, p0, p1, p2};
  return m;
}

FlutterPlatformViewMutation Scale(double sx, double sy) {
  return Transform(sx, 0, 0, 0, sy, 0);
}

FlutterPlatformViewMutation Translate(double tx, double ty) {
  return Transform(1, 0, tx, 0, 1, ty);
}

FlutterPlatformViewMutation Opacity(double o) {
  FlutterPlatformViewMutation m{};
  m.type = kFlutterPlatformViewMutationTypeOpacity;
  m.opacity = o;
  return m;
}

FlutterPlatformViewMutation RRect(double l,
                                  double t,
                                  double r,
                                  double b,
                                  double radius) {
  FlutterPlatformViewMutation m{};
  m.type = kFlutterPlatformViewMutationTypeClipRoundedRect;
  m.clip_rounded_rect.rect = {l, t, r, b};
  m.clip_rounded_rect.upper_left_corner_radius = {radius, radius};
  m.clip_rounded_rect.upper_right_corner_radius = {radius, radius};
  m.clip_rounded_rect.lower_right_corner_radius = {radius, radius};
  m.clip_rounded_rect.lower_left_corner_radius = {radius, radius};
  return m;
}

FlutterPlatformViewMutation Clip(double l, double t, double r, double b) {
  FlutterPlatformViewMutation m{};
  m.type = kFlutterPlatformViewMutationTypeClipRect;
  m.clip_rect = {l, t, r, b};
  return m;
}

// A platform view layer as the engine presents it: the mutations outermost
// first, the origin of the transformed bounds, and the size it was laid out
// at times the pixel ratio.
struct ViewLayer {
  std::vector<FlutterPlatformViewMutation> storage;
  std::vector<const FlutterPlatformViewMutation*> list;
  FlutterPlatformView view{};
  FlutterLayer layer{};

  ViewLayer(std::vector<FlutterPlatformViewMutation> mutations,
            FlutterPoint offset,
            FlutterSize size)
      : storage(std::move(mutations)) {
    for (const auto& m : storage) {
      list.push_back(&m);
    }
    view.struct_size = sizeof(FlutterPlatformView);
    view.identifier = 7;
    view.mutations_count = list.size();
    view.mutations = list.empty() ? nullptr : list.data();
    layer.struct_size = sizeof(FlutterLayer);
    layer.type = kFlutterLayerContentTypePlatformView;
    layer.platform_view = &view;
    layer.offset = offset;
    layer.size = size;
  }
};

}  // namespace

TEST(PlatformViewGeometry, NoMutationsIsUnscaled) {
  ViewLayer v({}, {10, 20}, {300, 200});
  EXPECT_TRUE(pv::AppScaleOf(v.layer, 1.0).IsIdentity());
  FlutterLayer out{};
  EXPECT_FALSE(pv::ScaleLayer(v.layer, 1.0, &out));
}

TEST(PlatformViewGeometry, PixelRatioAloneIsUnscaled) {
  // The framework's root layer scales by the pixel ratio; that is already in
  // the engine's size.
  ViewLayer v({Scale(1.5, 1.5), Translate(40, 60)}, {60, 90}, {300, 150});
  EXPECT_TRUE(pv::AppScaleOf(v.layer, 1.5).IsIdentity());
  FlutterLayer out{};
  EXPECT_FALSE(pv::ScaleLayer(v.layer, 1.5, &out));
  const FlutterSize c = pv::ContentSize(v.layer, 1.5);
  EXPECT_DOUBLE_EQ(c.width, 300);
  EXPECT_DOUBLE_EQ(c.height, 150);
}

TEST(PlatformViewGeometry, AppScaleSizesTheLayer) {
  // 100x50 logical at ratio 2, under Transform.scale(0.5): the engine reports
  // 200x100, the view draws at 100x50.
  ViewLayer v({Scale(2, 2), Translate(10, 20), Scale(0.5, 0.5)}, {20, 40},
              {200, 100});
  const auto s = pv::AppScaleOf(v.layer, 2.0);
  EXPECT_DOUBLE_EQ(s.x, 0.5);
  EXPECT_DOUBLE_EQ(s.y, 0.5);

  FlutterLayer out{};
  ASSERT_TRUE(pv::ScaleLayer(v.layer, 2.0, &out));
  EXPECT_DOUBLE_EQ(out.offset.x, 20);
  EXPECT_DOUBLE_EQ(out.offset.y, 40);
  EXPECT_DOUBLE_EQ(out.size.width, 100);
  EXPECT_DOUBLE_EQ(out.size.height, 50);
  EXPECT_EQ(out.platform_view, &v.view);
  EXPECT_EQ(out.type, kFlutterLayerContentTypePlatformView);

  // The producer keeps the laid-out size.
  const FlutterSize c = pv::ContentSize(out, 2.0);
  EXPECT_DOUBLE_EQ(c.width, 200);
  EXPECT_DOUBLE_EQ(c.height, 100);
}

TEST(PlatformViewGeometry, NonUniformScale) {
  ViewLayer v({Scale(2, 0.25)}, {0, 0}, {100, 100});
  FlutterLayer out{};
  ASSERT_TRUE(pv::ScaleLayer(v.layer, 1.0, &out));
  EXPECT_DOUBLE_EQ(out.size.width, 200);
  EXPECT_DOUBLE_EQ(out.size.height, 25);
}

TEST(PlatformViewGeometry, ContentSizeComesBackWhole) {
  // A third does not round-trip exactly in binary; the size must not come
  // back a hair under and truncate one pixel short.
  ViewLayer v({Scale(3, 3), Scale(1.0 / 3.0, 1.0 / 3.0)}, {0, 0}, {301, 7});
  FlutterLayer out{};
  ASSERT_TRUE(pv::ScaleLayer(v.layer, 3.0, &out));
  const FlutterSize c = pv::ContentSize(out, 3.0);
  EXPECT_EQ(static_cast<int>(c.width), 301);
  EXPECT_EQ(static_cast<int>(c.height), 7);
}

TEST(PlatformViewGeometry, UnknownPixelRatioLeavesTheLayer) {
  ViewLayer v({Scale(0.5, 0.5)}, {0, 0}, {100, 100});
  FlutterLayer out{};
  EXPECT_FALSE(pv::ScaleLayer(v.layer, 0.0, &out));
  EXPECT_FALSE(pv::ScaleLayer(v.layer, -1.0, &out));
  EXPECT_FALSE(pv::ScaleLayer(v.layer, std::nan(""), &out));
  EXPECT_DOUBLE_EQ(pv::ContentSize(v.layer, 0.0).width, 100);
}

TEST(PlatformViewGeometry, TransformsItCannotExpressAreLeftAlone) {
  FlutterLayer out{};
  // Rotation by 90 degrees.
  ViewLayer rot({Transform(0, -1, 0, 1, 0, 0)}, {0, 0}, {100, 50});
  EXPECT_FALSE(pv::ScaleLayer(rot.layer, 1.0, &out));
  // Skew.
  ViewLayer skew({Transform(1, 0.3, 0, 0, 1, 0)}, {0, 0}, {100, 50});
  EXPECT_FALSE(pv::ScaleLayer(skew.layer, 1.0, &out));
  // Mirror.
  ViewLayer mirror({Scale(-1, 1)}, {0, 0}, {100, 50});
  EXPECT_FALSE(pv::ScaleLayer(mirror.layer, 1.0, &out));
  // Perspective, even with a scale in the 2D part.
  ViewLayer persp({Transform(2, 0, 0, 0, 2, 0, 0.001, 0, 1)}, {0, 0},
                  {100, 50});
  EXPECT_FALSE(pv::ScaleLayer(persp.layer, 1.0, &out));
}

TEST(PlatformViewGeometry, BackingStoreLayersAreNotTouched) {
  FlutterLayer bs{};
  bs.struct_size = sizeof(FlutterLayer);
  bs.type = kFlutterLayerContentTypeBackingStore;
  bs.size = {640, 480};
  FlutterLayer out{};
  EXPECT_FALSE(pv::ScaleLayer(bs, 2.0, &out));
}

TEST(PlatformViewGeometry, FrameScalerPassesUnscaledFramesThrough) {
  ViewLayer v({Scale(2, 2)}, {0, 0}, {100, 100});
  FlutterLayer bs{};
  bs.type = kFlutterLayerContentTypeBackingStore;
  const FlutterLayer* frame[] = {&bs, &v.layer};
  pv::FrameScaler scaler;
  EXPECT_EQ(scaler.Scale(frame, 2, 2.0), frame);
  EXPECT_EQ(scaler.Scale(nullptr, 0, 2.0), nullptr);
}

TEST(PlatformViewGeometry, FrameScalerCopiesAScaledFrame) {
  FlutterLayer bs{};
  bs.type = kFlutterLayerContentTypeBackingStore;
  bs.size = {640, 480};
  ViewLayer plain({Scale(2, 2)}, {5, 5}, {100, 100});
  ViewLayer zoomed({Scale(2, 2), Scale(1.5, 1.5)}, {50, 60}, {200, 100});
  ViewLayer shrunk({Scale(2, 2), Scale(0.5, 0.25)}, {70, 80}, {40, 40});
  const FlutterLayer* frame[] = {&bs,           &plain.layer, nullptr,
                                 &zoomed.layer, &bs,          &shrunk.layer};
  pv::FrameScaler scaler;
  const FlutterLayer** out = scaler.Scale(frame, 6, 2.0);
  ASSERT_NE(out, frame);

  EXPECT_EQ(out[0]->type, kFlutterLayerContentTypeBackingStore);
  EXPECT_DOUBLE_EQ(out[0]->size.width, 640);
  EXPECT_DOUBLE_EQ(out[1]->size.width, 100);
  EXPECT_DOUBLE_EQ(out[1]->offset.x, 5);
  EXPECT_EQ(out[2], nullptr);
  EXPECT_DOUBLE_EQ(out[3]->offset.x, 50);
  EXPECT_DOUBLE_EQ(out[3]->size.width, 300);
  EXPECT_DOUBLE_EQ(out[3]->size.height, 150);
  EXPECT_EQ(out[3]->platform_view, &zoomed.view);
  EXPECT_DOUBLE_EQ(out[4]->size.height, 480);
  EXPECT_DOUBLE_EQ(out[5]->size.width, 20);
  EXPECT_DOUBLE_EQ(out[5]->size.height, 10);

  // The engine's layers are left as they were.
  EXPECT_DOUBLE_EQ(zoomed.layer.size.width, 200);

  // A later, shorter frame reuses the storage.
  const FlutterLayer* next[] = {&zoomed.layer};
  const FlutterLayer** out2 = scaler.Scale(next, 1, 2.0);
  ASSERT_NE(out2, next);
  EXPECT_DOUBLE_EQ(out2[0]->size.width, 300);
}

TEST(PlatformViewGeometry, NoClipMeansNoClipRect) {
  ViewLayer v({Scale(2, 2)}, {0, 0}, {100, 100});
  EXPECT_FALSE(pv::ClipRectOf(v.layer).has_value());
}

TEST(PlatformViewGeometry, AClipAroundTheViewIsNotReported) {
  // A list's clip that holds the whole view: nothing to crop.
  ViewLayer v({Scale(2, 2), Clip(0, 0, 400, 300), Translate(10, 10)}, {20, 20},
              {200, 200});
  EXPECT_FALSE(pv::ClipRectOf(v.layer).has_value());
}

TEST(PlatformViewGeometry, AClipIntoTheViewIsInOutputPixels) {
  // At pixel ratio 2, a list clipped to logical rows 0..100, and a 100 x 100
  // logical view scrolled to logical y = 50 inside it: its bottom half is cut.
  ViewLayer v({Scale(2, 2), Clip(0, 0, 400, 100), Translate(0, 50)}, {0, 100},
              {200, 200});
  const auto c = pv::ClipRectOf(v.layer);
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->x, 0);
  EXPECT_EQ(c->y, 0);
  EXPECT_EQ(c->w, 800);
  EXPECT_EQ(c->h, 200);
}

TEST(PlatformViewGeometry, AClipThatMissesTheViewIsEmpty) {
  // Scrolled clean out of the list's clip but still in the scene.
  ViewLayer v({Clip(0, 0, 100, 100), Clip(200, 200, 300, 300)}, {0, 0},
              {50, 50});
  const auto c = pv::ClipRectOf(v.layer);
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->w, 0);
  EXPECT_EQ(c->h, 0);
}

TEST(PlatformViewGeometry, AViewWithNoOpacityIsOpaque) {
  ViewLayer v({Scale(2, 2), Translate(10, 10)}, {20, 20}, {200, 200});
  EXPECT_EQ(pv::OpacityOf(v.layer), 1.0f);
  ViewLayer bare({}, {0, 0}, {10, 10});
  EXPECT_EQ(pv::OpacityOf(bare.layer), 1.0f);
}

TEST(PlatformViewGeometry, NestedOpacitiesMultiply) {
  // A FadeTransition at 0.5 inside an Opacity at 0.5.
  ViewLayer v({Scale(2, 2), Opacity(0.5), Translate(10, 10), Opacity(0.5)},
              {20, 20}, {200, 200});
  EXPECT_FLOAT_EQ(pv::OpacityOf(v.layer), 0.25f);
}

TEST(PlatformViewGeometry, OpacityIsKeptInRange) {
  // Within rounding of 1 reads as 1, so the opaque paths stay in use.
  ViewLayer near_one({Opacity(1.0 - 1e-9)}, {0, 0}, {10, 10});
  EXPECT_EQ(pv::OpacityOf(near_one.layer), 1.0f);
  ViewLayer zero({Opacity(0.0)}, {0, 0}, {10, 10});
  EXPECT_EQ(pv::OpacityOf(zero.layer), 0.0f);
  ViewLayer negative({Opacity(-0.5)}, {0, 0}, {10, 10});
  EXPECT_EQ(pv::OpacityOf(negative.layer), 0.0f);
  ViewLayer nan({Opacity(std::nan(""))}, {0, 0}, {10, 10});
  EXPECT_EQ(pv::OpacityOf(nan.layer), 1.0f);
}

TEST(PlatformViewGeometry, ABackingStoreIsOpaque) {
  ViewLayer v({Opacity(0.5)}, {0, 0}, {10, 10});
  v.layer.type = kFlutterLayerContentTypeBackingStore;
  EXPECT_EQ(pv::OpacityOf(v.layer), 1.0f);
}

TEST(PlatformViewGeometry, NoRoundedClipMeansNone) {
  ViewLayer v({Scale(2, 2), Clip(0, 0, 50, 50)}, {0, 0}, {100, 100});
  EXPECT_FALSE(pv::RoundedClipOf(v.layer).has_value());
}

TEST(PlatformViewGeometry, ARoundedClipThatCutsTheViewIsInOutputPixels) {
  // A 100 x 50 logical card with 8-point corners at pixel ratio 2, the view
  // filling it.
  ViewLayer v({Scale(2, 2), RRect(0, 0, 100, 50, 8)}, {0, 0}, {200, 100});
  const auto r = pv::RoundedClipOf(v.layer);
  ASSERT_TRUE(r.has_value());
  EXPECT_DOUBLE_EQ(r->rect.x, 0);
  EXPECT_DOUBLE_EQ(r->rect.w, 200);
  EXPECT_DOUBLE_EQ(r->rect.h, 100);
  for (const auto& corner : r->radii) {
    EXPECT_FLOAT_EQ(corner[0], 16);
    EXPECT_FLOAT_EQ(corner[1], 16);
  }
}

TEST(PlatformViewGeometry, ARoundedClipWhoseCornersMissTheViewIsNotReported) {
  // The view sits in the middle of a big rounded card, clear of its corners.
  ViewLayer v({RRect(0, 0, 400, 400, 20)}, {100, 100}, {200, 200});
  EXPECT_FALSE(pv::RoundedClipOf(v.layer).has_value());
}

TEST(PlatformViewGeometry, CornersOutsideTheClipRectAreNotReported) {
  // A rounded card, and inside it a plain clip that keeps its middle: the
  // part that shows never reaches a corner.
  ViewLayer v({RRect(0, 0, 100, 100, 10), Clip(20, 20, 80, 80)}, {0, 0},
              {100, 100});
  EXPECT_FALSE(pv::RoundedClipOf(v.layer).has_value());
  EXPECT_TRUE(pv::ClipRectOf(v.layer).has_value());
}

TEST(PlatformViewGeometry, SquareCornersAreNotReported) {
  ViewLayer v({RRect(0, 0, 100, 100, 0.25)}, {0, 0}, {100, 100});
  EXPECT_FALSE(pv::RoundedClipOf(v.layer).has_value());
}

namespace {

// Flutter's rotation by a quarter turn clockwise: (x, y) -> (-y, x).
FlutterPlatformViewMutation QuarterTurn() {
  return Transform(0, -1, 0, 1, 0, 0);
}

}  // namespace

TEST(PlatformViewGeometry, AnUntransformedViewNeedsNoMap) {
  ViewLayer plain({}, {10, 20}, {300, 200});
  EXPECT_FALSE(pv::TransformedViewOf(plain.layer, 1.0).has_value());
  // A positive scale is a rect on the output: ScaleLayer's job.
  ViewLayer scaled({Scale(2, 2), Translate(5, 5), Scale(0.5, 0.5)}, {10, 10},
                   {200, 100});
  EXPECT_FALSE(pv::TransformedViewOf(scaled.layer, 2.0).has_value());
}

TEST(PlatformViewGeometry, ARotatedViewMapsItsOwnPixels) {
  // 100x50 logical at ratio 2, turned a quarter turn about its origin at
  // (100, 50): the engine reports the bounds' origin and the unturned size.
  ViewLayer v({Scale(2, 2), Translate(100, 50), QuarterTurn()}, {100, 100},
              {200, 100});
  const auto t = pv::TransformedViewOf(v.layer, 2.0);
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->width, 200);
  EXPECT_EQ(t->height, 100);
  // Its own pixel (x, y) lands at (200 - y, 100 + x).
  for (const auto& [x, y] : {std::pair{0, 0}, std::pair{200, 0},
                             std::pair{0, 100}, std::pair{37, 61}}) {
    const auto p = t->map.Map(x, y);
    EXPECT_NEAR(p[0], 200 - y, 1e-9) << x << "," << y;
    EXPECT_NEAR(p[1], 100 + x, 1e-9) << x << "," << y;
  }
  EXPECT_FALSE(t->clip.has_value());
  // Not a scale: ScaleLayer leaves it alone.
  EXPECT_TRUE(pv::AppScaleOf(v.layer, 2.0).IsIdentity());
}

TEST(PlatformViewGeometry, AMirroredViewIsTransformed) {
  ViewLayer v({Translate(300, 0), Scale(-1, 1)}, {0, 0}, {300, 200});
  const auto t = pv::TransformedViewOf(v.layer, 1.0);
  ASSERT_TRUE(t.has_value());
  const auto p = t->map.Map(0, 0);
  EXPECT_DOUBLE_EQ(p[0], 300);
  EXPECT_DOUBLE_EQ(t->map.Map(300, 0)[0], 0);
}

TEST(PlatformViewGeometry, ATransformedViewIsClippedWhereItReallyIs) {
  // The quarter-turned view covers (100, 100) to (200, 300). The engine's
  // offset and size say (100, 100) to (300, 200), which the clip at y = 250
  // would miss.
  ViewLayer v(
      {Scale(2, 2), Clip(0, 0, 500, 125), Translate(100, 50), QuarterTurn()},
      {100, 100}, {200, 100});
  EXPECT_FALSE(pv::ClipRectOf(v.layer).has_value());
  const auto t = pv::TransformedViewOf(v.layer, 2.0);
  ASSERT_TRUE(t.has_value());
  ASSERT_TRUE(t->clip.has_value());
  EXPECT_EQ(t->clip->y + t->clip->h, 250);
}

TEST(PlatformViewGeometry, APerspectiveIsMapped) {
  // w = 1 + x / 1000 on a 100x100 view: its right edge is drawn at
  // 100 / 1.1 from the left, and half as far down at the bottom.
  ViewLayer persp({Transform(1, 0, 0, 0, 1, 0, 0.001, 0, 1)}, {0, 0},
                  {100, 100});
  EXPECT_TRUE(pv::IsTransformed(persp.layer));
  const auto t = pv::TransformedViewOf(persp.layer, 1.0);
  ASSERT_TRUE(t.has_value());
  EXPECT_NEAR(t->map.Map(100, 0)[0], 100 / 1.1, 1e-9);
  EXPECT_NEAR(t->map.Map(100, 100)[1], 100 / 1.1, 1e-9);
  // At a pixel ratio of 2 the view's own pixels are twice as many, and land
  // in the same places.
  ViewLayer at2({Scale(2, 2), Transform(1, 0, 0, 0, 1, 0, 0.001, 0, 1)}, {0, 0},
                {200, 200});
  const auto t2 = pv::TransformedViewOf(at2.layer, 2.0);
  ASSERT_TRUE(t2.has_value());
  EXPECT_NEAR(t2->map.Map(200, 0)[0], 2 * 100 / 1.1, 1e-9);
}

TEST(PlatformViewGeometry, AViewPastTheHorizonIsNotMapped) {
  // w = 1 - x / 50 is 0 halfway across and below it after.
  ViewLayer v({Transform(1, 0, 0, 0, 1, 0, -0.02, 0, 1)}, {0, 0}, {100, 100});
  EXPECT_FALSE(pv::TransformedViewOf(v.layer, 1.0).has_value());
}

TEST(PlatformViewGeometry, BadInputIsNotMapped) {
  ViewLayer turned({QuarterTurn()}, {0, 0}, {100, 100});
  EXPECT_FALSE(pv::TransformedViewOf(turned.layer, 0.0).has_value());
  EXPECT_FALSE(pv::TransformedViewOf(turned.layer, NAN).has_value());
  ViewLayer flat({Transform(1, 1, 0, 1, 1, 0)}, {0, 0}, {100, 100});
  EXPECT_FALSE(pv::TransformedViewOf(flat.layer, 1.0).has_value());
  ViewLayer empty({QuarterTurn()}, {0, 0}, {0, 100});
  EXPECT_FALSE(pv::TransformedViewOf(empty.layer, 1.0).has_value());
}

namespace {

// Flutter's rotation by @p degrees clockwise: (x, y) -> (c x - s y, s x + c y).
FlutterPlatformViewMutation Rotate(double degrees) {
  const double r = degrees * M_PI / 180.0;
  return Transform(std::cos(r), -std::sin(r), 0, std::sin(r), std::cos(r), 0);
}

}  // namespace

TEST(PlatformViewGeometry, AnAxisAlignedRoundedClipNeedsNoShapes) {
  ViewLayer v({RRect(0, 0, 100, 100, 10)}, {0, 0}, {100, 100});
  EXPECT_FALSE(pv::ClipShapesOf(v.layer).has_value());
  EXPECT_TRUE(pv::RoundedClipOf(v.layer).has_value());
  ViewLayer clipped({Clip(0, 0, 50, 100)}, {0, 0}, {100, 100});
  EXPECT_FALSE(pv::ClipShapesOf(clipped.layer).has_value());
}

TEST(PlatformViewGeometry, NestedRoundedClipsAreDrawnAsShapes) {
  // The rounded mask can take only one; both are kept as shapes, outermost
  // first, in the space they were pushed in.
  ViewLayer v(
      {RRect(0, 0, 100, 100, 40), Translate(10, 10), RRect(0, 0, 80, 80, 4)},
      {0, 0}, {100, 100});
  const auto shapes = pv::ClipShapesOf(v.layer);
  ASSERT_TRUE(shapes.has_value());
  ASSERT_EQ(shapes->count, 2U);
  EXPECT_DOUBLE_EQ(shapes->items[0].shape.rect.w, 100);
  EXPECT_FLOAT_EQ(shapes->items[0].shape.radii[0][0], 40);
  EXPECT_DOUBLE_EQ(shapes->items[1].shape.rect.w, 80);
  // The inner one's space is 10 pixels in.
  const auto q = shapes->items[1].to_shape.Map(10, 10);
  EXPECT_NEAR(q[0], 0, 1e-9);
  EXPECT_NEAR(q[1], 0, 1e-9);
  // (11, 11) is partly inside its corner but a pixel outside the outer arc.
  EXPECT_GT(ClipShapeCoverage(shapes->items[1], 11, 11), 0.0);
  EXPECT_EQ(ClipShapeCoverage(shapes->items[0], 11, 11), 0.0);
}

TEST(PlatformViewGeometry, ARoundedClipUnderARotationIsAShape) {
  // 40x20 about the origin, turned 30 degrees and moved to (50, 50).
  ViewLayer v({Translate(50, 50), Rotate(30), RRect(-20, -10, 20, 10, 5)},
              {30, 30}, {40, 40});
  const auto shapes = pv::ClipShapesOf(v.layer);
  ASSERT_TRUE(shapes.has_value());
  ASSERT_EQ(shapes->count, 1U);
  const ClipShape& s = shapes->items[0];
  // Output point for shape point (x, y), and back.
  const double c = std::cos(M_PI / 6);
  const double n = std::sin(M_PI / 6);
  const auto out = [&](double x, double y) {
    return std::array<double, 2>{50 + c * x - n * y, 50 + n * x + c * y};
  };
  for (const auto& [x, y] :
       {std::pair{0.0, 0.0}, std::pair{20.0, -10.0}, std::pair{-7.0, 3.0}}) {
    const auto p = out(x, y);
    const auto q = s.to_shape.Map(p[0], p[1]);
    EXPECT_NEAR(q[0], x, 1e-9);
    EXPECT_NEAR(q[1], y, 1e-9);
  }
  const auto mid = out(0, 0);
  EXPECT_EQ(ClipShapeCoverage(s, mid[0], mid[1]), 1.0);
  // The rect's own corner is two pixels outside the arc that rounds it.
  const auto corner = out(-20, -10);
  EXPECT_NEAR(ClipShapeDistance(s, corner[0], corner[1]), 5 * (M_SQRT2 - 1),
              1e-9);
  EXPECT_EQ(ClipShapeCoverage(s, corner[0], corner[1]), 0.0);
  // A side, rotated or not, is a pixel's distance a pixel away.
  const auto side = out(0, -11);
  EXPECT_NEAR(ClipShapeDistance(s, side[0], side[1]), 1.0, 1e-9);
}

TEST(PlatformViewGeometry, AClipRectUnderARotationIsAShape) {
  // The outer clip is a rect on screen and stays a scissor; the inner one is
  // turned, so its bounding box is not it.
  ViewLayer v({Clip(0, 0, 500, 500), Translate(50, 50), Rotate(30),
               Clip(-20, -10, 20, 10)},
              {30, 30}, {40, 40});
  const auto shapes = pv::ClipShapesOf(v.layer);
  ASSERT_TRUE(shapes.has_value());
  ASSERT_EQ(shapes->count, 1U);
  EXPECT_DOUBLE_EQ(shapes->items[0].shape.rect.x, -20);
  for (const auto& r : shapes->items[0].shape.radii) {
    EXPECT_EQ(r[0], 0.0f);
    EXPECT_EQ(r[1], 0.0f);
  }
}

TEST(PlatformViewGeometry, AClipUnderAPerspectiveMapsBack) {
  ViewLayer v(
      {Transform(1, 0, 0, 0, 1, 0, 0.002, 0, 1), RRect(0, 0, 100, 50, 10)},
      {0, 0}, {100, 50});
  const auto shapes = pv::ClipShapesOf(v.layer);
  ASSERT_TRUE(shapes.has_value());
  const ViewMap& m = shapes->items[0].to_shape;
  // (100, 50) is drawn at (100, 50) / 1.2.
  const auto q = m.Map(100 / 1.2, 50 / 1.2);
  EXPECT_NEAR(q[0], 100, 1e-9);
  EXPECT_NEAR(q[1], 50, 1e-9);
}

TEST(PlatformViewGeometry, MoreClipsThanShapesHoldAreLeftAsTheyWere) {
  std::vector<FlutterPlatformViewMutation> m;
  m.reserve(5);
  for (int k = 0; k < 5; ++k) {
    m.push_back(RRect(k, k, 100 - k, 100 - k, 10));
  }
  ViewLayer v(m, {0, 0}, {100, 100});
  EXPECT_FALSE(pv::ClipShapesOf(v.layer).has_value());
  EXPECT_TRUE(pv::RoundedClipOf(v.layer).has_value());
}
