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

#include "skybox.h"

#include "plugins/common/common.h"

namespace plugin_filament_view {

Skybox::Skybox(std::string assetPath, std::string url, std::string color)
    : assetPath_(std::move(assetPath)),
      url_(std::move(url)),
      color_(std::move(color)) {}

std::unique_ptr<Skybox> Skybox::Deserialize(
    const flutter::EncodableMap& params) {
  SPDLOG_TRACE("++Skybox::Skybox");
  std::optional<std::string> assetPath;
  std::optional<std::string> url;
  std::optional<std::string> color;
  std::optional<bool> showSun;
  std::optional<int32_t> skyboxType;

  for (auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "assetPath" && std::holds_alternative<std::string>(it.second)) {
      assetPath = std::get<std::string>(it.second);
    } else if (key == "url" && std::holds_alternative<std::string>(it.second)) {
      url = std::get<std::string>(it.second);
    } else if (key == "color" &&
               std::holds_alternative<std::string>(it.second)) {
      color = std::get<std::string>(it.second);
    } else if (key == "showSun" && std::holds_alternative<bool>(it.second)) {
      showSun = std::get<bool>(it.second);
    } else if (key == "skyboxType" &&
               std::holds_alternative<int32_t>(it.second)) {
      skyboxType = std::get<int32_t>(it.second);
    } else if (!it.second.IsNull()) {
      spdlog::debug("[SkyBox] Unhandled Parameter");
      plugin_common::Encodable::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }

  if (skyboxType.has_value()) {
    if (skyboxType == 1) {
      spdlog::debug("[Skybox] Type: KxtSkybox");
      return std::move(std::make_unique<KxtSkybox>(assetPath, url));
    } else if (skyboxType == 2) {
      spdlog::debug("[Skybox] Type: HdrSkybox");
      return std::move(std::make_unique<HdrSkybox>(assetPath, url, showSun));
    } else if (skyboxType == 3) {
      spdlog::debug("[Skybox] Type: ColorSkybox");
      return std::move(std::make_unique<ColorSkybox>(assetPath, url, color));
    }
  } else {
    spdlog::critical("[IndirectLight] Unknown Type: {}", skyboxType.value());
  }

  SPDLOG_TRACE("--Skybox::Skybox");
  return nullptr;
}

}  // namespace plugin_filament_view