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

#include <filament/LightManager.h>
#include "shell/platform/common/client_wrapper/include/flutter/encodable_value.h"

#include "core/scene/geometry/direction.h"
#include "core/scene/geometry/position.h"
#include "core/scene/light/light_manager.h"

class Direction;

class Position;

namespace plugin_filament_view {
class Light {
 public:
  explicit Light(float colorTemperature = 6'500.0f,
        float intensity = 100'000.0f,
        Direction direction = {0.0f, -1.0f, 0.0f},
        bool castShadows = true);

  explicit Light(const flutter::EncodableMap& params);

  void Print(const char* tag);

  static ::filament::LightManager::Type textToLightType(
      const std::string& type);

  static const char* lightTypeToText(::filament::LightManager::Type type);

  // Disallow copy and assign.
  Light(const Light&) = delete;

  Light& operator=(const Light&) = delete;

  friend class LightManager;

 private:
  ::filament::LightManager::Type type_;
  std::optional<std::string> color_;
  std::optional<float> colorTemperature_;
  std::optional<float> intensity_;
  std::unique_ptr<Position> position_;
  std::unique_ptr<Direction> direction_;
  std::optional<bool> castLight_;
  std::optional<bool> castShadows_;
  std::optional<float> falloffRadius_;
  std::optional<float> spotLightConeInner_;
  std::optional<float> spotLightConeOuter_;
  std::optional<float> sunAngularRadius_;
  std::optional<float> sunHaloSize_;
  std::optional<float> sunHaloFalloff_;
};
}  // namespace plugin_filament_view