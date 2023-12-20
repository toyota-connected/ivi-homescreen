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

#include "texture_sampler.h"

#include "logging/logging.h"
#include "utils.h"

namespace plugin_filament_view::material::texture {

TextureSampler::TextureSampler(const flutter::EncodableMap& params) {
  SPDLOG_TRACE("++TextureSampler::TextureSampler");
  for (auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "min" && std::holds_alternative<std::string>(it.second)) {
      min_ = std::get<std::string>(it.second);
    } else if (key == "mag" && std::holds_alternative<std::string>(it.second)) {
      mag_ = std::get<std::string>(it.second);
    } else if (key == "wrap" &&
               std::holds_alternative<std::string>(it.second)) {
      wrap_ = std::get<std::string>(it.second);
    } else if (key == "anisotropy" &&
               std::holds_alternative<double>(it.second)) {
      anisotropy_ = std::get<double>(it.second);
    } else if (!it.second.IsNull()) {
      spdlog::debug("[TextureSampler] Unhandled Parameter");
      Utils::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }
  SPDLOG_TRACE("--TextureSampler::TextureSampler");
}

void TextureSampler::Print(const char* tag) {
  spdlog::debug("++++++++");
  spdlog::debug("{} (TextureSampler)", tag);
  if (!min_.empty()) {
    spdlog::debug("\tmin: [{}]", min_);
  }
  if (!mag_.empty()) {
    spdlog::debug("\tmag: [{}]", mag_);
  }
  if (!wrap_.empty()) {
    spdlog::debug("\twrap: [{}]", wrap_);
  }
  if (anisotropy_.has_value()) {
    spdlog::debug("\tanisotropy: [{}]", anisotropy_.value());
  }
  spdlog::debug("++++++++");
}

}  // namespace plugin_filament_view::material::texture