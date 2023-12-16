
#pragma once

#include "shell/platform/common/client_wrapper/include/flutter/encodable_value.h"

#include <filament/Engine.h>
#include <gltfio/AssetLoader.h>
#include <gltfio/ResourceLoader.h>
#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"
#include "asio/io_context_strand.hpp"

#include "flutter_desktop_engine_state.h"
#include "models/scene/scene_controller.h"
#include "platform_views/platform_view.h"

namespace plugin_filament_view {

class FilamentScene {
 public:
  FilamentScene(PlatformView* platformView,
                FlutterDesktopEngineState* state,
                int32_t id,
                const std::vector<uint8_t>& params,
                const std::string& flutterAssetsPath);

  ~FilamentScene();

  SceneController* getSceneController() { return sceneController_.get(); };

 private:
  std::unique_ptr<SceneController> sceneController_;

  std::unique_ptr<Model> model_;
  std::unique_ptr<Scene> scene_;
  std::unique_ptr<std::vector<std::unique_ptr<Shape>>> shapes_{};
};

}  // namespace plugin_filament_view