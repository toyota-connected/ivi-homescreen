
#include "skybox_manager.h"

#include "logging/logging.h"


namespace plugin_filament_view {
SkyboxManager::SkyboxManager(void* context,
                             CustomModelViewer* model_viewer,
                             const std::string& flutter_assets_path)
    : context_(context),
      model_viewer_(model_viewer),
      flutterAssetsPath_(flutter_assets_path) {
  SPDLOG_TRACE("++SkyboxManager::SkyboxManager");
  SPDLOG_TRACE("--SkyboxManager::SkyboxManager");
}

void SkyboxManager::setDefaultSkybox(){

};
}  // namespace plugin_filament_view