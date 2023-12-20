/*
 * Copyright 2020-2023 Toyota Connected North America
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

#pragma once

#include "core/scene/scene_controller.h"
#include "viewer/custom_model_viewer.h"

namespace plugin_filament_view {

class CustomModelViewer;
class AnimationManager;

class AnimationManager {
 public:
  AnimationManager(CustomModelViewer* model_viewer);

  std::vector<std::string> getAnimationNames() { return animationNames_; }

  int32_t getAnimationCount() { return animationCount_; }

  int32_t getAnimationIndexByName(const std::string& /* name */) {
    // TODO
    return 0;
  }

  // Disallow copy and assign.
  AnimationManager(const AnimationManager&) = delete;
  AnimationManager& operator=(const AnimationManager&) = delete;

  friend class SceneController;

 private:
  CustomModelViewer* model_viewer_;
  int32_t animationCount_ = 0;
  std::vector<std::string> animationNames_;
};
}  // namespace plugin_filament_view