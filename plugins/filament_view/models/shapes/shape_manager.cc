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

#include "shape_manager.h"

#include "logging/logging.h"

namespace plugin_filament_view {
ShapeManager::ShapeManager(CustomModelViewer* model_viewer,
                           MaterialManager* material_manager)
    : model_viewer_(model_viewer), material_manager_(material_manager) {
  SPDLOG_TRACE("++ShapeManager::ShapeManager");
  SPDLOG_TRACE("--ShapeManager::ShapeManager");
}

void ShapeManager::createShapes(
    const std::vector<std::unique_ptr<Shape>>& shapes) {
  SPDLOG_DEBUG("ShapeManager::createShapes");
}
}  // namespace plugin_filament_view
