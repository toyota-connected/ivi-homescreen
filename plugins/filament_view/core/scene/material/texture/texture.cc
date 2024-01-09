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

#include <optional>

#include "plugins/common/common.h"

namespace plugin_filament_view {

static constexpr char kTypeColor[] = "COLOR";
static constexpr char kTypeNormal[] = "NORMAL";
static constexpr char kTypeData[] = "DATA";

Texture::Texture(TextureType type,
                 std::string assetPath,
                 std::string url,
                 TextureSampler* sampler)
    : type_(type),
      assetPath_(std::move(assetPath)),
      url_(std::move(url)),
      sampler_(sampler) {}

Texture::~Texture() {
  delete sampler_;
}

std::unique_ptr<Texture> Texture::Deserialize(
    const flutter::EncodableMap& params) {
  SPDLOG_TRACE("++Texture::Texture");
  std::optional<std::string> assetPath;
  std::optional<std::string> url;
  std::optional<TextureType> type;
  std::optional<std::unique_ptr<TextureSampler>> sampler;

  for (auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "assetPath" && std::holds_alternative<std::string>(it.second)) {
      assetPath = std::get<std::string>(it.second);
    } else if (key == "url" && std::holds_alternative<std::string>(it.second)) {
      url = std::get<std::string>(it.second);
    } else if (key == "type" &&
               std::holds_alternative<std::string>(it.second)) {
      type = getType(std::get<std::string>(it.second));
    } else if (key == "sampler" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      sampler = std::make_unique<TextureSampler>(
          std::get<flutter::EncodableMap>(it.second));
    } else if (!it.second.IsNull()) {
      spdlog::debug("[Texture] Unhandled Parameter");
      plugin_common::Encodable::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }
  if (!type.has_value()) {
    spdlog::error("[Texture] missing type");
    return nullptr;
  }

  SPDLOG_TRACE("--Texture::Texture");
  return std::make_unique<Texture>(
      type.value(), assetPath.has_value() ? std::move(assetPath.value()) : "",
      url.has_value() ? std::move(url.value()) : "",
      sampler.has_value() ? sampler.value().get() : nullptr);
}

void Texture::Print(const char* tag) {
  spdlog::debug("++++++++");
  spdlog::debug("{} (Texture)", tag);
  if (!assetPath_.empty()) {
    spdlog::debug("\tassetPath: [{}]", assetPath_);
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

Texture::TextureType Texture::getType(const std::string& type) {
  if (type == kTypeColor) {
    return TextureType::COLOR;
  } else if (type == kTypeNormal) {
    return TextureType::NORMAL;
  } else if (type == kTypeData) {
    return TextureType::DATA;
  }
  assert(false);
}

const char* Texture::getTextForType(Texture::TextureType type) {
  return (const char*[]){
      kTypeColor,
      kTypeNormal,
      kTypeData,
  }[type];
}

}  // namespace plugin_filament_view