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

#include "light_manager.h"

#include "logging/logging.h"

namespace plugin_filament_view {
LightManager::LightManager(CustomModelViewer* model_viewer)
    : model_viewer_(model_viewer) {
  SPDLOG_TRACE("++LightManager::LightManager");
  SPDLOG_TRACE("--LightManager::LightManager");
}

void LightManager::setDefaultLight() {
  SPDLOG_DEBUG("LightManager::setDefaultLight");
}

void LightManager::changeLight(Light* light) {
  light->Print("LightManager::changeLight");
};
}  // namespace plugin_filament_view