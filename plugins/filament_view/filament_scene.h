
#pragma once

#include <flutter/encodable_value.h>

#include <filament/Engine.h>
#include <gltfio/AssetLoader.h>
#include <gltfio/ResourceLoader.h>

#include "flutter_desktop_engine_state.h"
#include "platform_views/platform_view.h"
#include "scene_controller.h"

namespace plugin_filament_view {

class FilamentScene {
 public:
  FilamentScene(PlatformView *platformView,
                FlutterDesktopEngineState* state,
                int32_t id,
                const std::vector<uint8_t>& params,
                ::filament::Engine* engine,
                ::filament::gltfio::AssetLoader* assetLoader,
                ::filament::gltfio::ResourceLoader* resourceLoader,
                const std::string& flutterAssetsPath);

  ~FilamentScene();

 private:
  std::unique_ptr<SceneController> sceneController_;
};

}  // namespace plugin_filament_view