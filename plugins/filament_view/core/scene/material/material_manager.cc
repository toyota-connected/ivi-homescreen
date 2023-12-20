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

#include "material_manager.h"

#include "logging/logging.h"

namespace plugin_filament_view {
MaterialManager::MaterialManager(CustomModelViewer* model_viewer,
                                 const std::string& flutter_assets_path)
    : model_viewer_(model_viewer), flutterAssetsPath_(flutter_assets_path) {
  SPDLOG_TRACE("++MaterialManager::MaterialManager");
  SPDLOG_TRACE("--MaterialManager::MaterialManager");
}

}  // namespace plugin_filament_view