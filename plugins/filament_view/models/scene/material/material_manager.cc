
#include "material_manager.h"

#include "logging/logging.h"


namespace plugin_filament_view {
MaterialManager::MaterialManager(void* context,
                                 CustomModelViewer* model_viewer,
                                 const std::string& flutter_assets_path)
    : context_(context),
      model_viewer_(model_viewer),
      flutterAssetsPath_(flutter_assets_path) {
  SPDLOG_TRACE("++MaterialManager::MaterialManager");
  SPDLOG_TRACE("--MaterialManager::MaterialManager");
}

}  // namespace plugin_filament_view