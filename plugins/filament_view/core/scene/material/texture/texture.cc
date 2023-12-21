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

#include "texture.h"

#include <filesystem>

#include "logging/logging.h"
#include "utils.h"

namespace plugin_filament_view::material::texture {

Texture::Texture(const std::string& flutter_assets_path,
                 const flutter::EncodableMap& params)
    : flutterAssetsPath_(flutter_assets_path) {
  SPDLOG_TRACE("++Texture::Texture");
  for (auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "assetPath" && std::holds_alternative<std::string>(it.second)) {
      assetPath_ = std::get<std::string>(it.second);
    } else if (key == "url" && std::holds_alternative<std::string>(it.second)) {
      url_ = std::get<std::string>(it.second);
    } else if (key == "type" &&
               std::holds_alternative<std::string>(it.second)) {
      type_ = getType(std::get<std::string>(it.second));
    } else if (key == "sampler" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      sampler_ = std::make_unique<TextureSampler>(
          std::get<flutter::EncodableMap>(it.second));
    } else if (!it.second.IsNull()) {
      spdlog::debug("[Texture] Unhandled Parameter");
      Utils::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }
  SPDLOG_TRACE("--Texture::Texture");
}

void Texture::Print(const char* tag) {
  spdlog::debug("++++++++");
  spdlog::debug("{} (Texture)", tag);
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
  spdlog::debug("\ttype: {}", getTextForType(type_));
  if (sampler_) {
    sampler_->Print("\tsampler");
  }
  spdlog::debug("++++++++");
}

Texture::Type Texture::getType(const std::string& type) {
  if (type == kTypeColor) {
    return Type::color;
  } else if (type == kTypeNormal) {
    return Type::normal;
  } else if (type == kTypeData) {
    return Type::data;
  }
  assert(false);
}

const char* Texture::getTextForType(Texture::Type type) {
  return (const char*[]){
      kTypeColor,
      kTypeNormal,
      kTypeData,
  }[type];
}

}  // namespace plugin_filament_view::material::texture