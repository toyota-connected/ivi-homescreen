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

#include "direction.h"

#include "core/model/model.h"

#include "plugins/common/common.h"

namespace plugin_filament_view {
Direction::Direction(const flutter::EncodableMap& params) {
  SPDLOG_TRACE("++Direction::Direction");
  for (auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "x" && std::holds_alternative<double>(it.second)) {
      x_ = std::get<double>(it.second);
    } else if (key == "y" && std::holds_alternative<double>(it.second)) {
      y_ = std::get<double>(it.second);
    } else if (key == "z" && std::holds_alternative<double>(it.second)) {
      z_ = std::get<double>(it.second);
    } else if (!it.second.IsNull()) {
      spdlog::debug("[Direction] Unhandled Parameter");
      plugin_common::Encodable::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }
  SPDLOG_TRACE("--Direction::Direction");
}

void Direction::Print(const char* tag) {
  spdlog::debug("++++++++");
  spdlog::debug("{} (Direction) x: {}, y: {}, z: {}", tag, x_, y_, z_);
  spdlog::debug("++++++++");
}

}  // namespace plugin_filament_view
