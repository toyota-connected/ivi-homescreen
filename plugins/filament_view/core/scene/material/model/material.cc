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

#include "material.h"

#include <filesystem>

#include "plugins/common/common.h"

namespace plugin_filament_view {

Material::Material(const std::string& flutter_assets_path,
                   const flutter::EncodableMap& params)
    : flutterAssetsPath_(flutter_assets_path) {
  SPDLOG_TRACE("++Material::Material");
  for (auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "assetPath" && std::holds_alternative<std::string>(it.second)) {
      assetPath_ = std::get<std::string>(it.second);
    } else if (key == "url" && std::holds_alternative<std::string>(it.second)) {
      url_ = std::get<std::string>(it.second);
    } else if (key == "parameters" &&
               std::holds_alternative<flutter::EncodableList>(it.second)) {
      auto list = std::get<flutter::EncodableList>(it.second);
      for (const auto& it_ : list) {
        auto parameter = MaterialParameter::Deserialize(
            flutter_assets_path, std::get<flutter::EncodableMap>(it_));
        parameters_.push_back(std::move(parameter));
      }
    } else if (!it.second.IsNull()) {
      spdlog::debug("[Material] Unhandled Parameter");
      plugin_common::Encodable::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }
  SPDLOG_TRACE("--Material::Material");
}

Material::~Material() {
  for (auto& item : parameters_) {
    item.reset();
  }
  parameters_.clear();
}

void Material::Print(const char* tag) {
  spdlog::debug("++++++++");
  spdlog::debug("{} (Material)", tag);
  if (!assetPath_.empty()) {
    spdlog::debug("\tassetPath: [{}]", assetPath_);
    std::filesystem::path asset_folder(flutterAssetsPath_);
    spdlog::debug(
        "\tasset_path {} valid",
        std::filesystem::exists(asset_folder / assetPath_) ? "is" : "is not");
  }
  if (!url_.empty()) {
    spdlog::debug("\turl: [{}]", url_);
  }
  for (const auto& param : parameters_) {
    param->Print("\tparameter");
  }
  spdlog::debug("++++++++");
}

}  // namespace plugin_filament_view