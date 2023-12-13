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

#include <functional>

#include "platform_views/filament_view/models/state/model_state.h"
#include "platform_views/filament_view/models/scene/camera/camera_manager.h"
#include "shell/engine.h"

namespace view_filament_view {

class CameraManager;

class CustomModelViewer {
 public:
  explicit CustomModelViewer(void* parent);
  ~CustomModelViewer() = default;

  void setModelState(models::state::ModelState modelState);

  FML_DISALLOW_COPY_AND_ASSIGN(CustomModelViewer);

  [[nodiscard]] CameraManager* getCameraManager() const { return cameraManager_.get(); }

 private:
  void* parent_;
  models::state::ModelState modelState_;

  std::unique_ptr<CameraManager> cameraManager_;
};

}  // namespace view_filament_view::viewer
