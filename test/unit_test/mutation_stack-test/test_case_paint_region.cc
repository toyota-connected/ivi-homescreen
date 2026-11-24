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
#include <vector>

#include "gtest/gtest.h"
#include "view/paint_region.h"

namespace {

// A backing-store layer with @p rects as its paint region.
struct StoreLayer {
  std::vector<FlutterRect> rects;
  FlutterRegion region{};
  FlutterBackingStorePresentInfo info{};
  FlutterLayer layer{};

  explicit StoreLayer(std::vector<FlutterRect> r) : rects(std::move(r)) {
    region.struct_size = sizeof(FlutterRegion);
    region.rects_count = rects.size();
    region.rects = rects.empty() ? nullptr : rects.data();
    info.struct_size = sizeof(FlutterBackingStorePresentInfo);
    info.paint_region = &region;
    layer.struct_size = sizeof(FlutterLayer);
    layer.type = kFlutterLayerContentTypeBackingStore;
    layer.backing_store_present_info = &info;
    layer.size = {1280, 1440};
  }
};

}  // namespace

TEST(PaintRegion, NoRegionMeansTheWholeStore) {
  FlutterLayer layer{};
  layer.struct_size = sizeof(FlutterLayer);
  layer.type = kFlutterLayerContentTypeBackingStore;
  EXPECT_FALSE(PaintRects(layer, 1280, 1440).has_value());
  StoreLayer no_region({});
  no_region.info.paint_region = nullptr;
  EXPECT_FALSE(PaintRects(no_region.layer, 1280, 1440).has_value());
  // A platform view has no paint region at all.
  StoreLayer view({{0, 0, 10, 10}});
  view.layer.type = kFlutterLayerContentTypePlatformView;
  EXPECT_FALSE(PaintRects(view.layer, 1280, 1440).has_value());
}

TEST(PaintRegion, AnEmptyRegionMeansNothingPainted) {
  StoreLayer empty({});
  const auto r = PaintRects(empty.layer, 1280, 1440);
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->empty());
  // Rects with no area count as nothing too.
  StoreLayer flat({{100, 100, 100, 200}, {300, 300, 400, 300}});
  const auto f = PaintRects(flat.layer, 1280, 1440);
  ASSERT_TRUE(f.has_value());
  EXPECT_TRUE(f->empty());
}

TEST(PaintRegion, RectsAreRoundedOutAndClippedToTheStore) {
  StoreLayer s(
      {{10.4, 20.6, 170.2, 180.9}, {-5, -5, 20, 20}, {1270, 1430, 1300, 1500}});
  const auto r = PaintRects(s.layer, 1280, 1440);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->size(), 3U);
  EXPECT_EQ((*r)[0].x, 10);
  EXPECT_EQ((*r)[0].y, 20);
  EXPECT_EQ((*r)[0].w, 161);
  EXPECT_EQ((*r)[0].h, 161);
  EXPECT_EQ((*r)[1].x, 0);
  EXPECT_EQ((*r)[1].w, 20);
  EXPECT_EQ((*r)[2].x + (*r)[2].w, 1280);
  EXPECT_EQ((*r)[2].y + (*r)[2].h, 1440);
}

TEST(PaintRegion, ManyRectsBecomeTheirBoundingBox) {
  std::vector<FlutterRect> many;
  many.reserve(20);
  for (int i = 0; i < 20; ++i) {
    many.push_back({i * 10.0, 50.0, i * 10.0 + 5, 60.0});
  }
  StoreLayer s(many);
  const auto r = PaintRects(s.layer, 1280, 1440);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->size(), 1U);
  EXPECT_EQ((*r)[0].x, 0);
  EXPECT_EQ((*r)[0].y, 50);
  EXPECT_EQ((*r)[0].w, 195);
  EXPECT_EQ((*r)[0].h, 10);
}

TEST(PaintRegion, AnUnusableRegionMeansTheWholeStore) {
  StoreLayer nan({{0, 0, NAN, 10}});
  EXPECT_FALSE(PaintRects(nan.layer, 1280, 1440).has_value());
  StoreLayer missing({{0, 0, 10, 10}});
  missing.region.rects = nullptr;
  EXPECT_FALSE(PaintRects(missing.layer, 1280, 1440).has_value());
}

TEST(PaintRegion, CopiesArePaddedInsideTheStore) {
  const RectI r = PadRect({0, 10, 50, 1430}, 1, 1280, 1440);
  EXPECT_EQ(r.x, 0);
  EXPECT_EQ(r.y, 9);
  EXPECT_EQ(r.w, 51);
  EXPECT_EQ(r.h, 1431);
}

TEST(PaintRegion, TheCropSamplesJustTheRect) {
  const UvAffine uv = UvForRect({320, 360, 160, 160}, 1280, 1440);
  EXPECT_FLOAT_EQ(uv.U(0, 0), 0.25F);
  EXPECT_FLOAT_EQ(uv.V(0, 0), 0.25F);
  EXPECT_FLOAT_EQ(uv.U(1, 1), 0.375F);
  EXPECT_FLOAT_EQ(uv.V(1, 1), 520.0F / 1440.0F);
}
