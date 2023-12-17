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

#include <filament/Camera.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <utils/EntityManager.h>

#include "viewer/custom_model_viewer.h"

namespace plugin_filament_view {

class CustomModelViewer;

class CameraManager {
 public:
  CameraManager(CustomModelViewer* model_viewer);

  void updateCamera(Camera* camera);

  void lookAtDefaultPosition();

  void destroyCamera();

  [[nodiscard]] ::filament::Camera* getCamera() const { return camera_; }

  // Disallow copy and assign.
  CameraManager(const CameraManager&) = delete;
  CameraManager& operator=(const CameraManager&) = delete;

 private:
  static constexpr ::filament::math::float3 kCameraCenter = {0.0f, 0.0f, 0.0f};
  static constexpr ::filament::math::float3 kCameraUp = {0.0f, 1.0f, 0.0f};
  static constexpr float kCameraDist = 3.0f;

  // ImVec2: 2D vector used to store positions, sizes etc. [Compile-time
  // configurable type] This is a frequently used type in the API. Consider
  // using IM_VEC2_CLASS_EXTRA to create implicit cast from/to our preferred
  // type.
  struct ImVec2 {
    float x, y;
    ImVec2() { x = y = 0.0f; }
    ImVec2(float _x, float _y) {
      x = _x;
      y = _y;
    }
    float operator[](size_t idx) const {
      assert(idx <= 1);
      return (&x)[idx];
    }  // We very rarely use this [] operator, the assert overhead is fine.
    float& operator[](size_t idx) {
      assert(idx <= 1);
      return (&x)[idx];
    }  // We very rarely use this [] operator, the assert overhead is fine.
  };

  CustomModelViewer* model_viewer_;
  ::filament::Engine* engine_;
  ::filament::View* view_;
  ::filament::Camera* camera_ = nullptr;

  utils::Entity cameraEntity_;

  // Main display size, in pixels (generally == GetMainViewport()->Size)
  ImVec2 displaySize_;
  // Amount of pixels for each unit of DisplaySize. Based on
  // displayFramebufferScale_. Generally (1,1) on normal display, (2,2) on OSX
  // with Retina display.
  ImVec2 displayFramebufferScale_;
};
}  // namespace plugin_filament_view