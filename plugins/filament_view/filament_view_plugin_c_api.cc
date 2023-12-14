
#include "include/filament_view/filament_view_plugin_c_api.h"

#include <flutter/plugin_registrar.h>

#include "filament_view_plugin.h"

void FilamentViewPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar,
    int32_t id,
    std::string viewType,
    int32_t direction,
    double width,
    double height,
    const std::vector<uint8_t>& params,
    std::string assetDirectory,
    FlutterDesktopEngineRef engine) {
  plugin_filament_view::FilamentViewPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrar>(registrar),
      id, std::move(viewType), direction, width, height, params, assetDirectory,
      engine);
}
