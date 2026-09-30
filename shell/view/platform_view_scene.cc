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

#include "view/platform_view_scene.h"

#include <utility>
#include <vector>

PlatformViewScene::PlatformViewScene(Notify notify)
    : notify_(std::move(notify)) {}

void PlatformViewScene::Presented(const FlutterLayer** layers,
                                  const size_t count) {
  std::vector<std::pair<int64_t, bool>> changes;
  {
    std::lock_guard lock(mu_);
    scratch_.clear();
    for (size_t i = 0; i < count; ++i) {
      const FlutterLayer* layer = layers[i];
      if (layer != nullptr &&
          layer->type == kFlutterLayerContentTypePlatformView &&
          layer->platform_view != nullptr) {
        scratch_.insert(layer->platform_view->identifier);
      }
    }
    for (const int64_t id : in_scene_) {
      if (scratch_.count(id) == 0 && suspended_.insert(id).second) {
        changes.emplace_back(id, true);
      }
    }
    for (const int64_t id : scratch_) {
      if (suspended_.erase(id) != 0) {
        changes.emplace_back(id, false);
      }
    }
    in_scene_.swap(scratch_);
  }
  // Outside the lock: notify may post to another thread.
  for (const auto& [id, suspended] : changes) {
    notify_(id, suspended);
  }
}

bool PlatformViewScene::Suspended(const int64_t id) const {
  std::lock_guard lock(mu_);
  return suspended_.count(id) != 0;
}
