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

#include "scene.h"

#include "logging/logging.h"
#include "utils.h"

namespace plugin_filament_view {
Scene::Scene(void* parent,
             const std::string& flutter_assets_path,
             const flutter::EncodableValue& params)
    : parent_(parent), flutterAssetsPath_(flutter_assets_path) {
  for (auto& it : std::get<flutter::EncodableMap>(params)) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "skybox" &&
        std::holds_alternative<flutter::EncodableMap>(it.second)) {
      skybox_ =
          std::make_unique<Skybox>(parent, flutterAssetsPath_,
                                   std::get<flutter::EncodableMap>(it.second));
    } else if (key == "light" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      light_ =
          std::make_unique<Light>(parent, flutterAssetsPath_,
                                  std::get<flutter::EncodableMap>(it.second));
    } else if (key == "indirectLight" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      indirect_light_ = std::make_unique<IndirectLight>(
          parent, flutterAssetsPath_,
          std::get<flutter::EncodableMap>(it.second));
    } else if (key == "camera" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      camera_ =
          std::make_unique<Camera>(parent, flutterAssetsPath_,
                                   std::get<flutter::EncodableMap>(it.second));
    } else if (key == "ground" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      ground_ =
          std::make_unique<Ground>(parent, flutterAssetsPath_,
                                   std::get<flutter::EncodableMap>(it.second));
    } else if (!it.second.IsNull()) {
      spdlog::debug("[Scene] Unhandled Parameter");
      Utils::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }
}

void Scene::Print(const char* tag) const {
  spdlog::debug("++++++++");
  spdlog::debug("{} (Scene)", tag);
  if (skybox_.has_value()) {
    skybox_.value()->Print("\tskybox");
  }
  if (light_.has_value()) {
    light_.value()->Print("\tlight");
  }
  if (indirect_light_.has_value()) {
    indirect_light_.value()->Print("\tindirect_light");
  }
  if (camera_.has_value()) {
    camera_.value()->Print("\tcamera");
  }
  if (ground_.has_value()) {
    ground_.value()->Print("\tground");
  }
  spdlog::debug("++++++++");
}

}  // namespace plugin_filament_view
