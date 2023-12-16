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

#include "lens_projection.h"

#include "logging/logging.h"
#include "utils.h"

namespace plugin_filament_view {

LensProjection::LensProjection(void* parent,
                               const std::string& flutter_assets_path,
                               const flutter::EncodableMap& params)
    : parent_(parent), flutterAssetsPath_(flutter_assets_path) {
  SPDLOG_TRACE("++LensProjection::LensProjection");
  for (auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "focalLength" && std::holds_alternative<double>(it.second)) {
      focalLength_ = std::get<double>(it.second);
    } else if (key == "aspect" && std::holds_alternative<double>(it.second)) {
      aspect_ = std::get<double>(it.second);
    } else if (key == "near" && std::holds_alternative<double>(it.second)) {
      near_ = std::get<double>(it.second);
    } else if (key == "far" && std::holds_alternative<double>(it.second)) {
      far_ = std::get<double>(it.second);
    } else if (!it.second.IsNull()) {
      spdlog::debug("[LensProjection] Unhandled Parameter");
      Utils::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }
  SPDLOG_TRACE("--LensProjection::LensProjection");
}

void LensProjection::Print(const char* tag) {
  spdlog::debug("++++++++");
  spdlog::debug("{} (LensProjection)", tag);
  spdlog::debug("focalLength: {}", focalLength_);

  if (aspect_.has_value()) {
    spdlog::debug("aspect: {}", aspect_.value());
  }
  if (near_.has_value()) {
    spdlog::debug("near: {}", near_.value());
  }
  if (far_.has_value()) {
    spdlog::debug("far: {}", far_.value());
  }
  spdlog::debug("++++++++");
}

}  // namespace plugin_filament_view
