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

#include "filament_view_plugin.h"

#include <flutter/standard_message_codec.h>

#include "filament_scene.h"
#include "logging/logging.h"
#include "messages.g.h"

class FlutterView;

class Display;

namespace plugin_filament_view {

// static
void FilamentViewPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrar* registrar,
    int32_t id,
    std::string viewType,
    int32_t direction,
    double width,
    double height,
    const std::vector<uint8_t>& params,
    std::string assetDirectory,
    FlutterDesktopEngineRef engine) {
  auto plugin = std::make_unique<FilamentViewPlugin>(
      id, std::move(viewType), direction, width, height, params,
      std::move(assetDirectory), engine);

  FilamentViewApi::SetUp(registrar->messenger(), plugin.get(), id);
  ModelStateChannelApi::SetUp(registrar->messenger(), plugin.get(), id);
  SceneStateApi::SetUp(registrar->messenger(), plugin.get(), id);
  ShapeStateApi::SetUp(registrar->messenger(), plugin.get(), id);
  RendererChannelApi::SetUp(registrar->messenger(), plugin.get(), id);

  registrar->AddPlugin(std::move(plugin));
}

FilamentViewPlugin::FilamentViewPlugin(int32_t id,
                                       std::string viewType,
                                       int32_t direction,
                                       double width,
                                       double height,
                                       const std::vector<uint8_t>& params,
                                       std::string assetDirectory,
                                       FlutterDesktopEngineState* state)
    : PlatformView(id, std::move(viewType), direction, width, height),
      flutterAssetsPath_(std::move(assetDirectory)) {
  SPDLOG_TRACE("++FilamentViewPlugin::FilamentViewPlugin");
  filamentScene_ = std::make_unique<FilamentScene>(this, state, id, params,
                                                   flutterAssetsPath_);
  SPDLOG_TRACE("--FilamentViewPlugin::FilamentViewPlugin");
}

FilamentViewPlugin::~FilamentViewPlugin() = default;

void FilamentViewPlugin::Resize(double width, double height) {
  filamentScene_->getSceneController()->getModelViewer()->resize(width, height);
}

void FilamentViewPlugin::SetDirection(int32_t direction) {
  direction_ = direction;
  SPDLOG_TRACE("SetDirection: {}", direction_);
}

void FilamentViewPlugin::OnTouch(int32_t action, double x, double y) {
  filamentScene_->getSceneController()->getCameraManager()->onAction(action, x,
                                                                     y);
}

void FilamentViewPlugin::SetOffset(double left, double top) {
  filamentScene_->getSceneController()->getModelViewer()->setOffset(left, top);
}

void FilamentViewPlugin::Dispose(bool /* hybrid */) {
  filamentScene_.reset();
}

void FilamentViewPlugin::ChangeAnimationByIndex(
    const int32_t index,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeAnimationByName(
    std::string name,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::GetAnimationNames(
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::GetAnimationCount(
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::GetCurrentAnimationIndex(
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::GetAnimationNameByIndex(
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeSkyboxByAsset(
    std::string path,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeSkyboxByUrl(
    std::string url,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeSkyboxByHdrAsset(
    std::string path,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeSkyboxByHdrUrl(
    std::string url,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeSkyboxColor(
    std::string color,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeToTransparentSkybox(
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeLightByKtxAsset(
    std::string path,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeLightByKtxUrl(
    std::string url,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeLightByIndirectLight(
    std::string path,
    double intensity,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeLightByHdrUrl(
    std::string path,
    double intensity,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeToDefaultIndirectLight(
    const std::function<void(std::optional<FlutterError> reply)> result) {}

}  // namespace plugin_filament_view