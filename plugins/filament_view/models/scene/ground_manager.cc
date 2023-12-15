#include "ground_manager.h"

#include "logging/logging.h"

namespace plugin_filament_view {
GroundManager::GroundManager(CustomModelViewer* model_viewer,
                             const std::string& flutter_assets_path)
    : model_viewer_(model_viewer), flutterAssetsPath_(flutter_assets_path) {}

void GroundManager::createGround(Ground* ground) {
  SPDLOG_DEBUG("GroundManager::createGround");
}
}  // namespace plugin_filament_view