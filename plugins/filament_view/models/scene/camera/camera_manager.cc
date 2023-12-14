
#include "camera_manager.h"

#include "logging/logging.h"

namespace plugin_filament_view {
CameraManager::CameraManager(CustomModelViewer* model_viewer)
    : model_viewer_(model_viewer) {}

void CameraManager::updateCamera(Camera* camera) {
  SPDLOG_DEBUG("CameraManager::updateCamera");
}
}  // namespace plugin_filament_view