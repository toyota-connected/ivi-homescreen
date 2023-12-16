
#include "light_manager.h"

#include "logging/logging.h"

namespace plugin_filament_view {
LightManager::LightManager(CustomModelViewer* model_viewer)
    : model_viewer_(model_viewer) {
  SPDLOG_TRACE("++LightManager::LightManager");
  SPDLOG_TRACE("--LightManager::LightManager");
}

void LightManager::setDefaultLight() {
  SPDLOG_DEBUG("LightManager::setDefaultLight");
}

void LightManager::changeLight(Light* light) {
  light->Print("LightManager::changeLight");
};
}  // namespace plugin_filament_view