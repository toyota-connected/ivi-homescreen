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

#include "light.h"

#include "logging/logging.h"
#include "utils.h"


namespace plugin_filament_view {

Light::Light(void* parent,
             const std::string& flutter_assets_path,
             const flutter::EncodableMap& params)
    : parent_(parent), flutterAssetsPath_(flutter_assets_path) {
  for (auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "type" && std::holds_alternative<std::string>(it.second)) {
      type_ = textToLightType(std::get<std::string>(it.second));
    } else if (key == "color" &&
               std::holds_alternative<std::string>(it.second)) {
      auto ascii_hex = std::get<std::string>(it.second);
      color_ = std::stoul(ascii_hex, 0, 16);
    } else if (key == "colorTemperature" &&
               std::holds_alternative<double>(it.second)) {
      colorTemperature_ = std::get<double>(it.second);
    } else if (key == "intensity" &&
               std::holds_alternative<double>(it.second)) {
      intensity_ = std::get<double>(it.second);
    } else if (key == "position" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      position_ = std::make_unique<Position>(
          parent_, flutterAssetsPath_,
          std::get<flutter::EncodableMap>(it.second));
    } else if (key == "direction" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      direction_ = std::make_unique<Direction>(
          parent_, flutterAssetsPath_,
          std::get<flutter::EncodableMap>(it.second));
    } else if (key == "castLight" && std::holds_alternative<bool>(it.second)) {
      castLight_ = std::get<bool>(it.second);
    } else if (key == "castShadows" &&
               std::holds_alternative<bool>(it.second)) {
      castShadows_ = std::get<bool>(it.second);
    } else if (key == "falloffRadius" &&
               std::holds_alternative<double>(it.second)) {
      falloffRadius_ = std::get<double>(it.second);
    } else if (key == "spotLightConeInner" &&
               std::holds_alternative<double>(it.second)) {
      spotLightConeInner_ = std::get<double>(it.second);
    } else if (key == "spotLightConeOuter" && !it.second.IsNull() &&
               std::holds_alternative<double>(it.second)) {
      spotLightConeOuter_ = std::get<double>(it.second);
    } else if (key == "sunAngularRadius" && !it.second.IsNull() &&
               std::holds_alternative<double>(it.second)) {
      sunAngularRadius_ = std::get<double>(it.second);
    } else if (key == "sunHaloSize" && !it.second.IsNull() &&
               std::holds_alternative<double>(it.second)) {
      sunHaloSize_ = std::get<double>(it.second);
    } else if (key == "sunHaloFalloff" && !it.second.IsNull() &&
               std::holds_alternative<double>(it.second)) {
      sunHaloFalloff_ = std::get<double>(it.second);
    } else if (!it.second.IsNull()) {
      spdlog::debug("[Light] Unhandled Parameter");
      Utils::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }
}

void Light::Print(const char* tag) {
  spdlog::debug("++++++++");
  spdlog::debug("{} (Light)", tag);

  spdlog::debug("\ttype: {}", lightTypeToText(type_));
  if (color_.has_value()) {
    spdlog::debug("\tcolor: {}", color_.value());
  }
  if (colorTemperature_.has_value()) {
    spdlog::debug("\tcolorTemperature: {}", colorTemperature_.value());
  }
  if (intensity_.has_value()) {
    spdlog::debug("\tintensity: {}", intensity_.value());
  }
  if (position_.has_value()) {
    position_.value()->Print("\tposition");
  }
  if (direction_.has_value()) {
    direction_.value()->Print("\tposition");
  }
  if (castLight_.has_value()) {
    spdlog::debug("\tcastLight: {}", castLight_.value());
  }
  if (castShadows_.has_value()) {
    spdlog::debug("\tcastShadows: {}", castShadows_.value());
  }
  if (falloffRadius_.has_value()) {
    spdlog::debug("\tfalloffRadius: {}", falloffRadius_.value());
  }
  if (spotLightConeInner_.has_value()) {
    spdlog::debug("\tspotLightConeInner: {}", spotLightConeInner_.value());
  }
  if (spotLightConeOuter_.has_value()) {
    spdlog::debug("\tspotLightConeOuter: {}", spotLightConeOuter_.value());
  }
  if (sunAngularRadius_.has_value()) {
    spdlog::debug("\tsunAngularRadius: {}", sunAngularRadius_.value());
  }
  if (sunHaloSize_.has_value()) {
    spdlog::debug("\tsunHaloSize: {}", sunHaloSize_.value());
  }
  if (sunHaloFalloff_.has_value()) {
    spdlog::debug("\tsunHaloFalloff: {}", sunHaloFalloff_.value());
  }
  spdlog::debug("++++++++");
}

}  // namespace plugin_filament_view