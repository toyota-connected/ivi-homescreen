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

#include "animation.h"

#include <filesystem>

#include "core/scene/geometry/position.h"
#include "core/utils/deserialize.h"

#include "plugins/common/common.h"

namespace plugin_filament_view {
Animation::Animation(const std::string& flutter_assets_path,
                     const flutter::EncodableMap& params)
    : flutterAssetsPath_(flutter_assets_path) {
  for (auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "autoPlay" && std::holds_alternative<bool>(it.second)) {
      auto_play_ = std::get<bool>(it.second);
    } else if (key == "index" && std::holds_alternative<int32_t>(it.second)) {
      index_ = std::optional<int32_t>{std::get<int32_t>(it.second)};
    } else if (key == "name" &&
               std::holds_alternative<std::string>(it.second)) {
      name_ = std::get<std::string>(it.second);
    } else if (key == "assetPath" &&
               std::holds_alternative<std::string>(it.second)) {
      asset_path_ = std::get<std::string>(it.second);
    } else if (key == "centerPosition" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      center_position_ = std::make_unique<::filament::math::float3>(
          Deserialize::Format3(std::get<flutter::EncodableMap>(it.second)));
    } else if (!it.second.IsNull()) {
      spdlog::debug("[Animation] Unhandled Parameter");
      plugin_common::Encodable::PrintFlutterEncodableValue(key.c_str(),
                                                           it.second);
    }
  }
}

void Animation::Print(const char* tag) {
  spdlog::debug("++++++++");
  spdlog::debug("{} (Animation)", tag);
  spdlog::debug("\tname: [{}]", name_);
  if (index_.has_value()) {
    spdlog::debug("\tindex_: {}", index_.value());
  }
  spdlog::debug("\tautoPlay: {}", auto_play_);
  spdlog::debug("\tasset_path: [{}]", asset_path_);
  const std::filesystem::path asset_folder(flutterAssetsPath_);
  spdlog::debug(
      "\tasset_path {} valid",
      std::filesystem::exists(asset_folder / asset_path_) ? "is" : "is not");
  // TODO  center_position_->Print("\tcenterPosition:");
  spdlog::debug("++++++++");
}

}  // namespace plugin_filament_view