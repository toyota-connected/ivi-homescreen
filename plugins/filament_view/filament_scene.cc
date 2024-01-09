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

#include "filament_scene.h"

#include "shell/platform/common/client_wrapper/include/flutter/standard_message_codec.h"

#include "core/scene/scene_controller.h"
#include "plugins/common/common.h"

namespace plugin_filament_view {

FilamentScene::FilamentScene(PlatformView* platformView,
                             FlutterDesktopEngineState* state,
                             int32_t id,
                             const std::vector<uint8_t>& params,
                             const std::string& flutterAssetsPath) {
  SPDLOG_TRACE("++FilamentScene::FilamentScene");
  auto& codec = flutter::StandardMessageCodec::GetInstance();
  const auto decoded = codec.DecodeMessage(params.data(), params.size());
  const auto& creationParams =
      std::get_if<flutter::EncodableMap>(decoded.get());

  for (const auto& it : *creationParams) {
    if (it.second.IsNull())
      continue;
    auto key = std::get<std::string>(it.first);

    if (key == "model") {
      model_ = Model::Deserialize(flutterAssetsPath, it.second);
    } else if (key == "scene") {
      scene_ = std::make_unique<Scene>(flutterAssetsPath, it.second);
    } else if (key == "shapes" &&
               std::holds_alternative<flutter::EncodableList>(it.second)) {
      auto list = std::get<flutter::EncodableList>(it.second);
      for (const auto& it_ : list) {
        if (!it_.IsNull() &&
            std::holds_alternative<flutter::EncodableList>(it_)) {
          auto shape = std::make_unique<Shape>(
              flutterAssetsPath, std::get<flutter::EncodableMap>(it_));
          shapes_->emplace_back(shape.release());
        }
      }
    } else {
      spdlog::warn("[FilamentView] Unhandled Parameter");
      plugin_common::Encodable::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }
  sceneController_ = std::make_unique<SceneController>(
      platformView, state, flutterAssetsPath, model_.get(), scene_.get(),
      shapes_.get(), id);
  SPDLOG_TRACE("--FilamentScene::FilamentScene");
}

FilamentScene::~FilamentScene() {
  SPDLOG_TRACE("++FilamentScene::~FilamentScene");
  SPDLOG_TRACE("--FilamentScene::~FilamentScene");
};

}  // namespace plugin_filament_view
