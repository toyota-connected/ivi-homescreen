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

#include "position.h"

#include "logging/logging.h"
#include "utils.h"

namespace plugin_filament_view {

std::unique_ptr<Position> Position::Deserialize(const flutter::EncodableMap& params) {
  SPDLOG_TRACE("++Position::Deserialize");
  float x, y, z;
  for (auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "x" && std::holds_alternative<double>(it.second)) {
      x = static_cast<float>(std::get<double>(it.second));
    } else if (key == "y" && std::holds_alternative<double>(it.second)) {
      y = static_cast<float>(std::get<double>(it.second));
    } else if (key == "z" && std::holds_alternative<double>(it.second)) {
      z = static_cast<float>(std::get<double>(it.second));
    } else if (!it.second.IsNull()) {
      spdlog::debug("[Direction] Unhandled Parameter");
      Utils::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }
  SPDLOG_TRACE("--Position::Position");
  return std::move(std::make_unique<Position>(x, y ,z));
}

void Position::Print(const char* tag) const {
  spdlog::debug("++++++++");
  spdlog::debug("{} (Position)", tag);
  spdlog::debug("\tx: {}", x_);
  spdlog::debug("\ty: {}", y_);
  spdlog::debug("\tz: {}", z_);
  spdlog::debug("++++++++");
}

}  // namespace plugin_filament_view
