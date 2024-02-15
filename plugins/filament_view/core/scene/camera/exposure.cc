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

#include "plugins/common/common.h"

namespace plugin_filament_view {

Exposure::Exposure(const flutter::EncodableMap& params) {
  SPDLOG_TRACE("++Exposure::Exposure");
  for (auto& it : params) {
    auto key = std::get<std::string>(it.first);
    if (key == "aperture") {
      if (std::holds_alternative<double>(it.second)) {
        aperture_ = std::get<double>(it.second);
      } else if (std::holds_alternative<std::monostate>(it.second)) {
        aperture_ = 16.0f;
      }
    } else if (key == "sensitivity") {
      if (std::holds_alternative<double>(it.second)) {
        sensitivity_ = std::get<double>(it.second);
      } else if (std::holds_alternative<std::monostate>(it.second)) {
        sensitivity_ = 100.0f;
      }
    } else if (key == "shutterSpeed") {
      if (std::holds_alternative<double>(it.second)) {
        shutterSpeed_ = std::get<double>(it.second);
      } else if (std::holds_alternative<double>(it.second)) {
        shutterSpeed_ = 1.0f / 125.0f;
      }
    } else if (key == "exposure") {
      if (std::holds_alternative<double>(it.second)) {
        exposure_ = std::get<double>(it.second);
      }
    }
  }
  SPDLOG_TRACE("--Exposure::Exposure");
  Print("Exposure");
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