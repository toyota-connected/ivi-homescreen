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

#include "indirect_light.h"

#include <filesystem>

#include "logging/logging.h"
#include "utils.h"

namespace plugin_filament_view {

IndirectLight::IndirectLight(void* parent,
                             const std::string& flutter_assets_path,
                             const flutter::EncodableMap& params)
    : parent_(parent), flutterAssetsPath_(flutter_assets_path) {
  for (auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "assetPath" && std::holds_alternative<std::string>(it.second)) {
      assetPath_ = std::get<std::string>(it.second);
    } else if (key == "url" && std::holds_alternative<std::string>(it.second)) {
      url_ = std::get<std::string>(it.second);
    } else if (key == "intensity" &&
               std::holds_alternative<double>(it.second)) {
      intensity_ = std::get<double>(it.second);
    } else if (key == "lightType" &&
               std::holds_alternative<int32_t>(it.second)) {
      lightType_ = std::get<int32_t>(it.second);
    } else if (!it.second.IsNull()) {
      spdlog::debug("[IndirectLight] Unhandled Parameter");
      Utils::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }
}

void IndirectLight::Print(const char* tag) {
  spdlog::debug("++++++++");
  spdlog::debug("{} (IndirectLight)", tag);
  if (!assetPath_.empty()) {
    spdlog::debug("\tassetPath: [{}]", assetPath_);
    std::filesystem::path asset_folder(flutterAssetsPath_);
    spdlog::debug(
        "\tasset_path {} valid",
        std::filesystem::exists(asset_folder / assetPath_) ? "is" : "is not");
  }
  if (!url_.empty()) {
    spdlog::debug("\turl: {}", url_);
  }
  if (intensity_.has_value()) {
    spdlog::debug("\tintensity: {}", intensity_.value());
  }
  if (lightType_.has_value()) {
    spdlog::debug("\tlightType: {}", lightType_.value());
  }
  spdlog::debug("++++++++");
}

}  // namespace plugin_filament_view