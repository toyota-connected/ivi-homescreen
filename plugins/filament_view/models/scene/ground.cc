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

#include "ground.h"

#include "logging/logging.h"
#include "utils.h"

namespace plugin_filament_view {

Ground::Ground(void* parent,
               const std::string& flutter_assets_path,
               const flutter::EncodableMap& params)
    : parent_(parent), flutterAssetsPath_(flutter_assets_path) {
  for (auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "centerPosition" &&
        std::holds_alternative<flutter::EncodableMap>(it.second)) {
      center_position_ = std::make_unique<Position>(
          parent, flutterAssetsPath_,
          std::get<flutter::EncodableMap>(it.second));
    } else if (key == "normal" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      normal_ = std::make_unique<Direction>(
          parent, flutterAssetsPath_,
          std::get<flutter::EncodableMap>(it.second));
    } else if (key == "isBelowModel" &&
               std::holds_alternative<bool>(it.second)) {
      isBelowModel_ = std::get<bool>(it.second);
    } else if (key == "size" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      size_ =
          std::make_unique<Size>(parent, flutterAssetsPath_,
                                 std::get<flutter::EncodableMap>(it.second));
    } else if (key == "material" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      material_ = std::make_unique<Material>(
          parent, flutterAssetsPath_,
          std::get<flutter::EncodableMap>(it.second));
    } else if (!it.second.IsNull()) {
      spdlog::debug("[Ground] Unhandled Parameter");
      Utils::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }
}

void Ground::Print(const char* tag) {
  spdlog::debug("++++++++");
  spdlog::debug("{} (Ground)", tag);
  if (center_position_.has_value()) {
    center_position_.value()->Print("\tcenter_position");
  }
  if (normal_.has_value()) {
    normal_.value()->Print("\tnormal");
  }
  spdlog::debug("\tisBelowModel: {}", isBelowModel_);
  if (size_.has_value()) {
    size_.value()->Print("\tsize");
  }
  if (material_.has_value()) {
    material_.value()->Print("\tmaterial");
  }
  spdlog::debug("++++++++");
}

}  // namespace plugin_filament_view