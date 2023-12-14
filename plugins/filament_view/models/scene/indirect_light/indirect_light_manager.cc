
#include "indirect_light_manager.h"

namespace plugin_filament_view {
IndirectLightManager::IndirectLightManager(
    void *context,
    CustomModelViewer* model_viewer,
    const std::string& flutter_assets_path)
    : context_(context), model_viewer_(model_viewer), flutterAssetsPath_(flutter_assets_path) {}

void IndirectLightManager::setDefaultIndirectLight(IndirectLight *indirect_light){
    indirect_light->Print("IndirectLightManager::setDefaultIndirectLight");
};
}  // namespace plugin_filament_view