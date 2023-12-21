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

#include "core/model/glb_loader.h"
#include "core/model/gltf_loader.h"
#include "core/model/model.h"
#include "core/model/model_loader.h"
#include "core/scene/camera/camera_manager.h"
#include "core/scene/scene.h"
#include "core/shapes/shape.h"
#include "flutter_desktop_plugin_registrar.h"
#include "models/state/model_state.h"
#include "models/state/scene_state.h"
#include "models/state/shape_state.h"
#include "platform_views/platform_view.h"
#include "settings.h"

namespace plugin_filament_view {

class CameraManager;

class GlbLoader;

class GltfLoader;

class ModelLoader;

class Model;

class Scene;

using models::state::ModelState;
using models::state::SceneState;
using models::state::ShapeState;

class CustomModelViewer {
 public:
  static constexpr ::filament::float3 kDefaultObjectPosition = {0.0f, 0.0f,
                                                                -4.0f};

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

  void setLightState(models::state::SceneState sceneState);

  void setSkyboxState(models::state::SceneState sceneState);

  void destroyIndirectLight();

  void destroySkybox();

  // Disallow copy and assign.
  CustomModelViewer(const CustomModelViewer&) = delete;

  CustomModelViewer& operator=(const CustomModelViewer&) = delete;

  [[nodiscard]] ::filament::Engine* getFilamentEngine() const {
    return fengine_;
  }

  [[nodiscard]] ::filament::View* getFilamentView() const { return fview_; }

  [[nodiscard]] ::filament::Scene* getFilamentScene() const { return fscene_; }

  [[nodiscard]] ::filament::Skybox* getFilamentSkybox() const {
    return fskybox_;
  }

  [[nodiscard]] ::filament::Renderer* getFilamentRenderer() const {
    return frenderer_;
  }

  [[nodiscard]] plugin_filament_view::Scene* getScene() const { return scene_; }

  ModelLoader* getModelLoader() const { return modelLoader_.get(); }

  [[nodiscard]] GlbLoader* getGlbModelLoader() const {
    return glbLoader_.get();
  }

  [[nodiscard]] GltfLoader* getGltfModelLoader() const {
    return gltfLoader_.get();
  }

  void setAnimator(filament::gltfio::Animator* animator) {
    fanimator_ = animator;
  }

  [[nodiscard]] const asio::io_context::strand& getStrandContext() const {
    return *strand_;
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

  [[maybe_unused]] FlutterDesktopEngineState* state_;
  const std::string flutterAssetsPath_;
  filament::viewer::Settings settings_;
  filament::gltfio::FilamentAsset* asset_{};
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

  plugin_filament_view::Scene* scene_{};

  ::filament::Engine* fengine_{};
  ::filament::Scene* fscene_{};
  ::filament::View* fview_{};
  filament::Skybox* fskybox_ = nullptr;
  ::filament::Renderer* frenderer_{};
  ::filament::SwapChain* fswapChain_{};

  ::filament::gltfio::Animator* fanimator_;

  ModelState currentModelState_;
  [[maybe_unused]] SceneState currentSkyboxState_;
  [[maybe_unused]] SceneState currentLightState_;
  [[maybe_unused]] SceneState currentGroundState_;
  [[maybe_unused]] ShapeState currentShapesState_;

  std::unique_ptr<ModelLoader> modelLoader_;
  std::unique_ptr<GlbLoader> glbLoader_;
  std::unique_ptr<GltfLoader> gltfLoader_;

  CameraManager* cameraManager_{};

  static void OnFrame(void* data, wl_callback* callback, uint32_t time);

  static const wl_callback_listener frame_listener;

  void DrawFrame(uint32_t time);

  void setupView();
};

}  // namespace plugin_filament_view
