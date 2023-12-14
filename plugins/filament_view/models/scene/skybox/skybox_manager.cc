
#include "skybox_manager.h"

namespace plugin_filament_view {
SkyboxManager::SkyboxManager(void* context,
                             CustomModelViewer* model_viewer,
                             const std::string& flutter_assets_path)
    : context_(context),
      model_viewer_(model_viewer),
      flutterAssetsPath_(flutter_assets_path) {}

void SkyboxManager::setDefaultSkybox(){

};
}  // namespace plugin_filament_view