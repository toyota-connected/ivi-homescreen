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

#include "indirect_light.h"

#include "plugins/common/common.h"

namespace plugin_filament_view {

class DefaultIndirectLight;

class KtxIndirectLight;

class HdrIndirectLight;

IndirectLight::IndirectLight(std::string assetPath,
                             std::string url,
                             float intensity)
    : assetPath_(std::move(assetPath)),
      url_(std::move(url)),
      intensity_(intensity) {}

IndirectLight::~IndirectLight() = default;

std::unique_ptr<IndirectLight> IndirectLight::Deserialize(
    const flutter::EncodableMap& params) {
  SPDLOG_TRACE("++IndirectLight::Deserialize");

  std::optional<int32_t> type;
  std::optional<std::string> assetPath;
  std::optional<std::string> url;
  std::optional<double> intensity;

  for (auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "assetPath" && std::holds_alternative<std::string>(it.second)) {
      assetPath = std::get<std::string>(it.second);
    } else if (key == "url" && std::holds_alternative<std::string>(it.second)) {
      url = std::get<std::string>(it.second);
    } else if (key == "intensity" &&
               std::holds_alternative<double>(it.second)) {
      intensity = std::get<double>(it.second);
    } else if (key == "lightType" &&
               std::holds_alternative<int32_t>(it.second)) {
      type = std::get<int32_t>(it.second);
    } else if (!it.second.IsNull()) {
      spdlog::debug("[IndirectLight] Unhandled Parameter");
      plugin_common::Encodable::PrintFlutterEncodableValue(key.c_str(),
                                                           it.second);
    }
  }

  if (type.has_value()) {
    if (type == 1) {
      spdlog::debug("[IndirectLight] Type: KtxIndirectLight");
      return std::move(std::make_unique<KtxIndirectLight>(
          std::move(assetPath), std::move(url), intensity));
    } else if (type == 2) {
      spdlog::debug("[IndirectLight] Type: HdrIndirectLight");
      return std::move(std::make_unique<HdrIndirectLight>(
          std::move(assetPath), std::move(url), intensity));
    } else if (type == 3) {
      spdlog::debug("[IndirectLight] Type: DefaultIndirectLight");
      return std::move(std::make_unique<DefaultIndirectLight>());
    }
  } else {
    spdlog::critical("[IndirectLight] Unknown Type: {}", type.value());
  }

  SPDLOG_TRACE("--IndirectLight::Deserialize");
  return nullptr;
}

}  // namespace plugin_filament_view