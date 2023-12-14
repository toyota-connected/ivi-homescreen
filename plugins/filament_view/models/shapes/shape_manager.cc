#include "shape_manager.h"

#include "logging/logging.h"

namespace plugin_filament_view {
ShapeManager::ShapeManager(CustomModelViewer* model_viewer,
                           MaterialManager* material_manager)
    : model_viewer_(model_viewer), material_manager_(material_manager) {}

void ShapeManager::createShapes(
    const std::vector<std::unique_ptr<plugin_filament_view::Shape>>& shapes) {
  SPDLOG_DEBUG("ShapeManager::createShapes");
}
}  // namespace plugin_filament_view
