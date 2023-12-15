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

#include <functional>

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/View.h>
#include <gltfio/Animator.h>
#include <gltfio/AssetLoader.h>
#include <gltfio/ResourceLoader.h>
#include <wayland-client.h>

#include "models/scene/camera/camera_manager.h"
#include "models/state/model_state.h"

#include "flutter_desktop_plugin_registrar.h"
#include "models/model/model.h"
#include "models/scene/scene.h"
#include "models/shapes/shape.h"
#include "models/state/model_state.h"
#include "models/state/scene_state.h"
#include "models/state/shape_state.h"
#include "platform_views/platform_view.h"

class CameraManager;

namespace plugin_filament_view {

using models::state::ModelState;
using models::state::SceneState;
using models::state::ShapeState;

class CustomModelViewer {
 public:
  explicit CustomModelViewer(PlatformView *platformView,
                             FlutterDesktopEngineState* state,
                             ::filament::Engine* engine,
                             ::filament::gltfio::AssetLoader* assetLoader,
                             filament::gltfio::ResourceLoader* resourceLoader);
  ~CustomModelViewer();

  void setModelState(models::state::ModelState modelState);

  // Disallow copy and assign.
  CustomModelViewer(const CustomModelViewer&) = delete;
  CustomModelViewer& operator=(const CustomModelViewer&) = delete;

  [[nodiscard]] CameraManager* getCameraManager() const {
    return cameraManager_.get();
  }

 private:
  FlutterDesktopEngineState* state_;
  wl_display* display_;
  wl_surface* surface_;
  wl_surface* parent_surface_;
  wl_callback* callback_;
  wl_subsurface* subsurface_;

  struct _native_window {
    struct wl_display* display;
    struct wl_surface* surface;
    uint32_t width;
    uint32_t height;
  } native_window_{};

  ::filament::gltfio::Animator* animator_;

  ::filament::Engine* engine_;
  ::filament::Scene* scene_;
  ::filament::View* view_;
  ::filament::Renderer* renderer_;
  ::filament::SwapChain* swapChain_;

  ModelState currentModelState_;
  SceneState currentSkyboxState_;
  SceneState currentLightState_;
  SceneState currentGroundState_;
  ShapeState currentShapesState_;

  std::unique_ptr<CameraManager> cameraManager_;
  ::filament::gltfio::AssetLoader* assetLoader_;
  ::filament::gltfio::ResourceLoader* resourceLoader_;

  static void OnFrame(void* data, wl_callback* callback, uint32_t time);
  static const wl_callback_listener frame_listener;
  void DrawFrame(uint32_t time);

  void setupView();
};

}  // namespace plugin_filament_view
