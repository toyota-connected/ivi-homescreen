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

#include "shell/platform/common/client_wrapper/include/flutter/encodable_value.h"

#include <optional>

namespace plugin_filament_view {

class Direction {
 public:
  Direction(float x, float y, float z) : x_(x), y_(y), z_(z){};

  Direction(const flutter::EncodableMap& params);

  ::filament::math::float3 toFloatArray() { return {x_, y_, z_}; }

  void Print(const char* tag);

  // Disallow copy and assign.
  Direction(const Direction&) = delete;

  Direction& operator=(const Direction&) = delete;

  friend class LightManager;

  friend class Light;

 private:
  float x_;
  float y_;
  float z_;
};

}  // namespace plugin_filament_view
