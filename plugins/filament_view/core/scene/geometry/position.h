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

#include <math/vec3.h>

#include "core/scene/camera/camera_manager.h"
#include "core/scene/light/light.h"
#include "core/scene/light/light_manager.h"
#include "shell/platform/common/client_wrapper/include/flutter/encodable_value.h"

#include <optional>

class CameraManager;

class LightManager;

class Light;

namespace plugin_filament_view {

class Position {
 public:
  explicit Position(float x = 0.0f, float y = 0.0f, float z = 0.0f)
      : x_(x), y_(y), z_(z) {}

  static std::unique_ptr<Position> Deserialize(
      const flutter::EncodableMap& params);

  // getter methods
  [[nodiscard]] float getX() const { return x_; }

  [[nodiscard]] float getY() const { return y_; }

  [[nodiscard]] float getZ() const { return z_; }

  // setter methods
  void setX(float newX) { x_ = newX; }

  void setY(float newY) { y_ = newY; }

  void setZ(float newZ) { z_ = newZ; }

  [[nodiscard]] filament::math::float3 toFloatArray() const {
    return {x_, y_, z_};
  }

  void Print(const char* tag) const;

  // Disallow copy and assign.
  Position(const Position&) = delete;

  Position& operator=(const Position&) = delete;

  friend class CameraManager;

  friend class LightManager;

  friend class Light;

 private:
  float x_, y_, z_;
};

}  // namespace plugin_filament_view
