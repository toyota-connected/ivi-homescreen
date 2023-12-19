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

#ifndef FLUTTER_PLUGIN_FILAMENT_VIEW_PLUGIN_H_
#define FLUTTER_PLUGIN_FILAMENT_VIEW_PLUGIN_H_

#include <memory>

#include <flutter/event_channel.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>
#include <wayland-client.h>

#include "filament_scene.h"
#include "flutter_desktop_engine_state.h"
#include "flutter_homescreen.h"
#include "messages.g.h"
#include "platform_views/platform_view.h"
#include "view/flutter_view.h"
#include "wayland/display.h"

namespace plugin_filament_view {

class FilamentViewPlugin : public flutter::Plugin,
                           public FilamentViewApi,
                           public ModelStateChannelApi,
                           public SceneStateApi,
                           public ShapeStateApi,
                           public RendererChannelApi,
                           public PlatformView {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar,
                                    int32_t id,
                                    std::string viewType,
                                    int32_t direction,
                                    double width,
                                    double height,
                                    const std::vector<uint8_t>& params,
                                    std::string assetDirectory,
                                    FlutterDesktopEngineRef engine);

  FilamentViewPlugin(int32_t id,
                     std::string viewType,
                     int32_t direction,
                     double width,
                     double height,
                     const std::vector<uint8_t>& params,
                     std::string assetDirectory,
                     FlutterDesktopEngineState* state);

  ~FilamentViewPlugin() override;

  void Resize(double width, double height) override;

  void SetDirection(int32_t direction) override;

  void SetOffset(double left, double top) override;

  void OnTouch(int32_t action, double x, double y) override;

  void Dispose(bool hybrid) override;

  void ChangeAnimationByIndex(
      const int32_t index,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeAnimationByName(
      std::string name,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void GetAnimationNames(
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void GetAnimationCount(
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void GetCurrentAnimationIndex(
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void GetAnimationNameByIndex(
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeSkyboxByAsset(
      std::string path,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeSkyboxByUrl(
      std::string url,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeSkyboxByHdrAsset(
      std::string path,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeSkyboxByHdrUrl(
      std::string url,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeSkyboxColor(
      std::string color,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeToTransparentSkybox(
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeLightByKtxAsset(
      std::string path,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeLightByKtxUrl(
      std::string url,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeLightByIndirectLight(
      std::string path,
      double intensity,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeLightByHdrUrl(
      std::string path,
      double intensity,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeToDefaultIndirectLight(
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;

  // Disallow copy and assign.
  FilamentViewPlugin(const FilamentViewPlugin&) = delete;
  FilamentViewPlugin& operator=(const FilamentViewPlugin&) = delete;

 private:
  const std::string flutterAssetsPath_;

  std::unique_ptr<FilamentScene> filamentScene_;
};

}  // namespace plugin_filament_view

#endif  // FLUTTER_PLUGIN_FILAMENT_VIEW_PLUGIN_H_
