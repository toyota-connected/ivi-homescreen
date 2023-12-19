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
#include <future>

#include <cstdint>

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/SwapChain.h>
#include <filament/View.h>
#include <gltfio/Animator.h>
#include <gltfio/AssetLoader.h>
#include <gltfio/NodeManager.h>
#include <gltfio/ResourceLoader.h>
#include <wayland-client.h>
#include <asio/io_context_strand.hpp>

#include "models/scene/camera/camera_manager.h"
#include "models/state/model_state.h"

#include "flutter_desktop_plugin_registrar.h"
#include "models/model/model.h"
#include "models/model/model_loader.h"
#include "models/scene/scene.h"
#include "models/shapes/shape.h"
#include "models/state/model_state.h"
#include "models/state/scene_state.h"
#include "models/state/shape_state.h"
#include "platform_views/platform_view.h"
#include "settings.h"

class CameraManager;
class ModelLoader;

namespace plugin_filament_view {

using models::state::ModelState;
using models::state::SceneState;
using models::state::ShapeState;

class CustomModelViewer {
 public:
  CustomModelViewer(PlatformView* platformView,
                    FlutterDesktopEngineState* state,
                    std::string flutterAssetsPath);
  ~CustomModelViewer();

  std::future<bool> Initialize(PlatformView* platformView);

  void setCameraManager(CameraManager* cameraManager) {
    cameraManager_ = cameraManager;
  };

  void setModelState(models::state::ModelState modelState);
  void setGroundState(models::state::SceneState sceneState);

#if 0   // TODO
  const ::filament::math::mat4f& getModelTransform() {
    return modelLoader_->getModelTransform();
  };
#endif  // TODO

  // Disallow copy and assign.
  CustomModelViewer(const CustomModelViewer&) = delete;
  CustomModelViewer& operator=(const CustomModelViewer&) = delete;

  [[nodiscard]] ::filament::Engine* getFilamentEngine() const {
    return fengine_;
  }

  [[nodiscard]] ::filament::View* getFilamentView() const { return fview_; }

  [[nodiscard]] ::filament::Scene* getFilamentScene() const { return fscene_; }

  [[nodiscard]] ::filament::Renderer* getFilamentRenderer() const {
    return frenderer_;
  }

  [[nodiscard]] Scene* getScene() const { return scene_; }

  std::string loadModel(Model* model);

  [[nodiscard]] ModelLoader* getModelLoader() const {
    return modelLoader_.get();
  }

  void setAnimator(filament::gltfio::Animator* animator) {
    fanimator_ = animator;
  }

  [[nodiscard]] asio::io_context::strand* getStrandContext() const {
    return strand_.get();
  }

  filament::viewer::Settings& getSettings() { return settings_; }

  filament::gltfio::FilamentAsset* getAsset() { return asset_; }

  bool getActualSize() { return actualSize; }

  void setInitialized() {
    initialized_ = true;
    OnFrame(this, nullptr, 0);
  }

  pthread_t getFilamentApiThreadId() const { return filament_api_thread_id_; }

  std::string getAssetPath() const { return flutterAssetsPath_; }

  void setOffset(double left, double top);

  void resize(double width, double height);

 private:
  static constexpr bool actualSize = false;
  static constexpr bool originIsFarAway = false;
  static constexpr float originDistance = 1.0f;

  FlutterDesktopEngineState* state_;
  const std::string flutterAssetsPath_;
  filament::viewer::Settings settings_;
  filament::gltfio::FilamentAsset* asset_;
  int32_t left_;
  int32_t top_;

  bool initialized_{};

  std::thread filament_api_thread_;
  pthread_t filament_api_thread_id_{};
  std::unique_ptr<asio::io_context> io_context_;
  asio::executor_work_guard<decltype(io_context_->get_executor())> work_;
  std::unique_ptr<asio::io_context::strand> strand_;

  wl_display* display_{};
  wl_surface* surface_{};
  wl_surface* parent_surface_{};
  wl_callback* callback_;
  wl_subsurface* subsurface_{};

  struct _native_window {
    struct wl_display* display;
    struct wl_surface* surface;
    uint32_t width;
    uint32_t height;
  } native_window_{};

  Scene* scene_{};

  ::filament::Engine* fengine_{};
  ::filament::Scene* fscene_{};
  ::filament::View* fview_{};
  ::filament::Renderer* frenderer_{};
  ::filament::SwapChain* fswapChain_{};

  ::filament::gltfio::Animator* fanimator_;

  ModelState currentModelState_;
  SceneState currentSkyboxState_;
  SceneState currentLightState_;
  SceneState currentGroundState_;
  ShapeState currentShapesState_;

  filament::Skybox* skybox_ = nullptr;

  std::unique_ptr<ModelLoader> modelLoader_;

  CameraManager* cameraManager_;

  static void OnFrame(void* data, wl_callback* callback, uint32_t time);
  static const wl_callback_listener frame_listener;
  void DrawFrame(uint32_t time);

  void setupView();
};

}  // namespace plugin_filament_view
