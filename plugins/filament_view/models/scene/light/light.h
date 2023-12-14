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

#include <flutter/encodable_value.h>

#include "models/scene/geometry/direction.h"
#include "models/scene/geometry/position.h"

namespace plugin_filament_view {
class Light {
 public:
  enum LightType {
    /// Directional light that also draws a sun's disk in the sky.
    sun,
    /// Directional light, emits light in a given direction.
    directional,
    /// Point light, emits light from a position, in all directions.
    point,
    /// Physically correct spot light.
    focusedSpot,
    /// Spot light with coupling of outer cone and illumination disabled.
    spot,
  };
  Light(void* parent,
        const std::string& flutter_assets_path,
        const flutter::EncodableMap& params);
  void Print(const char* tag);

  static LightType textToLightType(const std::string& type) {
    if (type == "SUN") {
      return LightType::sun;
    } else if (type == "DIRECTIONAL") {
      return LightType::directional;
    } else if (type == "POINT") {
      return LightType::point;
    } else if (type == "FOCUSED_SPOT") {
      return LightType::focusedSpot;
    } else if (type == "SPOT") {
      return LightType::spot;
    }
    return LightType::directional;
  }

  static const char* lightTypeToText(LightType type) {
    switch (type) {
      case LightType::sun:
        return "SUN";
      case LightType::directional:
        return "DIRECTIONAL";
      case LightType::point:
        return "POINT";
      case LightType::focusedSpot:
        return "FOCUSED_SPOT";
      case LightType::spot:
        return "SPOT";
      default:
        return "DIRECTIONAL";
    }
  }

  // Disallow copy and assign.
  Light(const Light&) = delete;
  Light& operator=(const Light&) = delete;

 private:
  void* parent_;
  const std::string& flutterAssetsPath_;

  LightType type_;
  std::optional<int32_t> color_;
  std::optional<double> colorTemperature_;
  std::optional<double> intensity_;
  std::optional<std::unique_ptr<Position>> position_;
  std::optional<std::unique_ptr<Direction>> direction_;
  std::optional<bool> castLight_;
  std::optional<bool> castShadows_;
  std::optional<double> falloffRadius_;
  std::optional<double> spotLightConeInner_;
  std::optional<double> spotLightConeOuter_;
  std::optional<double> sunAngularRadius_;
  std::optional<double> sunHaloSize_;
  std::optional<double> sunHaloFalloff_;
};
}  // namespace plugin_filament_view