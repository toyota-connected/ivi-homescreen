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
