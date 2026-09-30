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
#include "view/platform_view_scene.h"

namespace {

using Change = std::pair<int64_t, bool>;

// A frame's layers: a backing store, then a platform view per id.
class Frame {
 public:
  explicit Frame(const std::vector<int64_t>& ids) {
    backing_store_.struct_size = sizeof(FlutterLayer);
    backing_store_.type = kFlutterLayerContentTypeBackingStore;
    layers_.push_back(&backing_store_);
    views_.reserve(ids.size());
    platform_layers_.reserve(ids.size());
    for (const int64_t id : ids) {
      FlutterPlatformView view{};
      view.struct_size = sizeof(FlutterPlatformView);
      view.identifier = id;
      views_.push_back(view);
    }
    for (auto& view : views_) {
      FlutterLayer layer{};
      layer.struct_size = sizeof(FlutterLayer);
      layer.type = kFlutterLayerContentTypePlatformView;
      layer.platform_view = &view;
      platform_layers_.push_back(layer);
    }
    for (auto& layer : platform_layers_) {
      layers_.push_back(&layer);
    }
  }

  void PresentTo(PlatformViewScene& scene) {
    scene.Presented(layers_.data(), layers_.size());
  }

 private:
  FlutterLayer backing_store_{};
  std::vector<FlutterPlatformView> views_;
  std::vector<FlutterLayer> platform_layers_;
  std::vector<const FlutterLayer*> layers_;
};

}  // namespace

TEST(PlatformViewScene, SuspendsViewsThatLeaveAndResumesThemOnReturn) {
  std::vector<Change> changes;
  PlatformViewScene scene([&](const int64_t id, const bool suspended) {
    changes.emplace_back(id, suspended);
  });

  // First frames: nothing left, nothing to tell.
  Frame({1, 2}).PresentTo(scene);
  Frame({1, 2}).PresentTo(scene);
  EXPECT_TRUE(changes.empty());

  // 2 goes offstage.
  Frame({1}).PresentTo(scene);
  EXPECT_EQ(changes, (std::vector<Change>{{2, true}}));
  EXPECT_TRUE(scene.Suspended(2));
  EXPECT_FALSE(scene.Suspended(1));

  // Still gone: told once.
  Frame({1}).PresentTo(scene);
  EXPECT_EQ(changes.size(), 1u);

  // Back.
  Frame({1, 2}).PresentTo(scene);
  EXPECT_EQ(changes, (std::vector<Change>{{2, true}, {2, false}}));
  EXPECT_FALSE(scene.Suspended(2));
}

TEST(PlatformViewScene, NewViewsAreNotResumed) {
  std::vector<Change> changes;
  PlatformViewScene scene([&](const int64_t id, const bool suspended) {
    changes.emplace_back(id, suspended);
  });
  Frame({}).PresentTo(scene);
  Frame({7}).PresentTo(scene);
  EXPECT_TRUE(changes.empty());
  // No platform views at all: every one there was leaves.
  Frame({}).PresentTo(scene);
  EXPECT_EQ(changes, (std::vector<Change>{{7, true}}));
}
