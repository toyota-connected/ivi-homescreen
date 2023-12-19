/*
* Copyright 2020-2023 Toyota Connected North America
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

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