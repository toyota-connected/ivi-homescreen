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

#include "model.h"

#include <filesystem>

#include "logging/logging.h"
#include "utils.h"

namespace plugin_filament_view {

Model::Model(void* parent,
             const std::string& flutter_assets_path,
             const flutter::EncodableValue& params)
    : flutterAssetsPath_(flutter_assets_path) {
  SPDLOG_TRACE("++Model::Model");
  for (auto& it : std::get<flutter::EncodableMap>(params)) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "animation" &&
        std::holds_alternative<flutter::EncodableMap>(it.second)) {
      animation_ = std::make_unique<Animation>(
          this, flutterAssetsPath_, std::get<flutter::EncodableMap>(it.second));
    } else if (key == "assetPath" &&
               std::holds_alternative<std::string>(it.second)) {
      assetPath_ = std::get<std::string>(it.second);
    } else if (key == "centerPosition" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      center_position_ = std::make_unique<Position>(
          std::get<flutter::EncodableMap>(it.second));
    } else if (key == "fallback" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      fallback_ = std::make_unique<Model>(
          parent, flutterAssetsPath_,
          flutter::EncodableValue(std::get<flutter::EncodableMap>(it.second)));
    } else if (key == "isGlb" && std::holds_alternative<bool>(it.second)) {
      is_glb_ = std::optional<bool>{std::get<bool>(it.second)};
    } else if (key == "scale" && std::holds_alternative<double>(it.second)) {
      scale_ = std::get<double>(it.second);
    } else if (key == "url" && std::holds_alternative<std::string>(it.second)) {
      url_ = std::get<std::string>(it.second);
    } else if (key == "pathPrefix" &&
               std::holds_alternative<std::string>(it.second)) {
      pathPrefix_ = std::get<std::string>(it.second);
    } else if (key == "pathPostfix" &&
               std::holds_alternative<std::string>(it.second)) {
      pathPostfix_ = std::get<std::string>(it.second);
    } else if (key == "scene" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      scene_ = std::make_unique<Scene>(parent, flutterAssetsPath_, it.second);
    } else if (!it.second.IsNull()) {
      spdlog::debug("[Model] Unhandled Parameter");
      Utils::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }
  SPDLOG_TRACE("--Model::Model");
}

void Model::Print(const char* tag) {
  spdlog::debug("++++++++");
  spdlog::info("{} (Model)", tag);
  spdlog::info("\tis_glb: {}", is_glb_.value_or(false));
  spdlog::info("\tasset_path: {}", assetPath_);
  const std::filesystem::path asset_folder(flutterAssetsPath_);
  spdlog::info(
      "\tasset_path {} valid",
      std::filesystem::exists(asset_folder / assetPath_) ? "is" : "is not");
  spdlog::info("\turl: [{}]", url_);
  spdlog::info("\tpathPrefix: [{}]", pathPrefix_);
  spdlog::info("\tpathPostfix: [{}]", pathPostfix_);
  if (fallback_.has_value()) {
    fallback_.value()->Print("\tfallback");
  }
  spdlog::info("\tscale: {}", scale_);
  if (center_position_.has_value()) {
    center_position_.value()->Print("\tcenter_position");
  }
  if (animation_.has_value()) {
    animation_.value()->Print("\tanimation");
  }
  if (scene_.has_value()) {
    scene_.value()->Print("\tscene");
  }
  spdlog::debug("++++++++");
}
}  // namespace plugin_filament_view
