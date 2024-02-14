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

#include "shape.h"

#include "core/utils/deserialize.h"
#include "plugins/common/common.h"

namespace plugin_filament_view {

Shape::Shape(int32_t id,
             ::filament::math::float3 centerPosition,
             ::filament::math::float3 normal,
             Material material) {
  SPDLOG_TRACE("++Shape::Shape");

  SPDLOG_TRACE("--Shape::Shape");
}

Shape::Shape(const std::string& flutter_assets_path,
             const flutter::EncodableMap& params) {
  SPDLOG_TRACE("++Shape::Shape");
  for (auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    spdlog::debug("Shape: {}", key);

    if (key == "id" && std::holds_alternative<int>(it.second)) {
      type_ = std::get<int>(it.second);
    } else if (key == "type" && std::holds_alternative<int32_t>(it.second)) {
      type_ = std::get<int32_t>(it.second);
    } else if (key == "centerPosition" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      centerPosition_ = std::make_unique<::filament::math::float3>(
          Deserialize::Format3(std::get<flutter::EncodableMap>(it.second)));
    } else if (key == "normal" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      normal_ = std::make_unique<::filament::math::float3>(
          Deserialize::Format3(std::get<flutter::EncodableMap>(it.second)));
    } else if (key == "material" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      material_ = std::make_unique<Material>(
          flutter_assets_path, std::get<flutter::EncodableMap>(it.second));
    } else if (!it.second.IsNull()) {
      spdlog::debug("[Shape] Unhandled Parameter");
      plugin_common::Encodable::PrintFlutterEncodableValue(key.c_str(),
                                                           it.second);
    }
  }
  SPDLOG_TRACE("--Shape::Shape");
}

void Shape::Print(const char* tag) const {
  spdlog::debug("++++++++");
  spdlog::debug("{} (Shape)", tag);
#if 0
  if (centerPosition_.has_value()) {
    centerPosition_.value()->Print("\tcenterPosition");
  }
  if (normal_.has_value()) {
    normal_.value()->Print("\tnormal");
  }
#endif
  if (material_.has_value()) {
    material_.value()->Print("\tsize");
  }
  spdlog::debug("++++++++");
}

}  // namespace plugin_filament_view