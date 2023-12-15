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

#include "camera.h"
#include "viewer/custom_model_viewer.h"

namespace plugin_filament_view {

class CustomModelViewer;

class CameraManager {
 public:
  explicit CameraManager(CustomModelViewer* model_viewer);

  void updateCamera(Camera* camera);

  void lookAtDefaultPosition();

  void destroyCamera();

  // Disallow copy and assign.
  CameraManager(const CameraManager&) = delete;
  CameraManager& operator=(const CameraManager&) = delete;

 private:
  CustomModelViewer* model_viewer_;
};
}  // namespace plugin_filament_view