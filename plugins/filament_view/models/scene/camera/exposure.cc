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

#include "exposure.h"

#include "logging/logging.h"
#include "models/scene/scene.h"
#include "utils.h"

namespace plugin_filament_view {

Exposure::Exposure(void* parent,
                   const std::string& flutter_assets_path,
                   const flutter::EncodableMap& params)
    : parent_(parent), flutterAssetsPath_(flutter_assets_path) {
  for (auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "aperture" && std::holds_alternative<double>(it.second)) {
      aperture_ = std::get<double>(it.second);
    } else if (key == "sensitivity" &&
               std::holds_alternative<double>(it.second)) {
      sensitivity_ = std::get<double>(it.second);
    } else if (key == "shutterSpeed" &&
               std::holds_alternative<double>(it.second)) {
      shutterSpeed_ = std::get<double>(it.second);
    } else if (key == "exposure" && std::holds_alternative<double>(it.second)) {
      exposure_ = std::get<double>(it.second);
    } else if (!it.second.IsNull()) {
      spdlog::debug("[Exposure] Unhandled Parameter");
      Utils::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }
}

void Exposure::Print(const char* tag) {
  spdlog::debug("++++++++");
  spdlog::debug("{} (Exposure)", tag);
  if (aperture_.has_value()) {
    spdlog::debug("\taperture: {}", aperture_.value());
  }
  if (sensitivity_.has_value()) {
    spdlog::debug("\tsensitivity: {}", sensitivity_.value());
  }
  if (shutterSpeed_.has_value()) {
    spdlog::debug("\tshutterSpeed: {}", shutterSpeed_.value());
  }
  if (exposure_.has_value()) {
    spdlog::debug("\texposure: {}", exposure_.value());
  }
  spdlog::debug("++++++++");
}

}  // namespace plugin_filament_view