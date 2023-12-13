
#include "light_manager.h"

namespace view_filament_view {
LightManager::LightManager(
    CustomModelViewer* model_viewer)
    : model_viewer_(model_viewer) {}

void LightManager::setDefaultLight(){
  SPDLOG_DEBUG("LightManager::setDefaultLight");
}

void LightManager::changeLight(Light* light){
  light->Print("LightManager::changeLight");
};
}  // namespace view_filament_view