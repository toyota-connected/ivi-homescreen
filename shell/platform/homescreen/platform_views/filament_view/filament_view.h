/*
 * Copyright 2023 Toyota Connected North America
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

#include <shell/platform/embedder/embedder.h>

#include <memory>
#include <optional>
#include <vector>

#include <filament/Engine.h>
#include <filament/View.h>
#include <gltfio/AssetLoader.h>
#include <gltfio/MaterialProvider.h>
#include <gltfio/ResourceLoader.h>

#include "models/model/animation_manager.h"
#include "models/model/glb/loader/glb_loader.h"
#include "models/model/gltf/loader/gltf_loader.h"
#include "models/model/model.h"
#include "models/scene/camera/camera_manager.h"
#include "models/scene/ground_manager.h"
#include "models/scene/indirect_light/indirect_light_manager.h"
#include "models/scene/light/light_manager.h"
#include "models/scene/material/material_manager.h"
#include "models/scene/scene.h"
#include "models/scene/skybox/skybox_manager.h"
#include "models/shapes/shape.h"
#include "models/shapes/shape_manager.h"
#include "platform_views/platform_view.h"
#include "shell/engine.h"
#include "viewer/custom_model_viewer.h"

extern "C" {
extern const uint8_t UBERARCHIVE_PACKAGE[];
extern int UBERARCHIVE_DEFAULT_OFFSET;
extern size_t UBERARCHIVE_DEFAULT_SIZE;
}
#define UBERARCHIVE_DEFAULT_DATA \
  (UBERARCHIVE_PACKAGE + UBERARCHIVE_DEFAULT_OFFSET)

namespace view_filament_view {

using namespace view_filament_view;
using namespace view_filament_view::material::texture;
using namespace view_filament_view::material::texture;

class FilamentView final : public PlatformView {
 public:
  FilamentView(int32_t id,
               std::string viewType,
               int32_t direction,
               double width,
               double height,
               const std::vector<uint8_t>& params,
               std::string assetDirectory,
               FlutterView* view);
  ~FilamentView() override;

  void Resize(double width, double height) override;

  void SetDirection(int32_t direction) override;

  void SetOffset(double left, double top) override;

  void Dispose(bool hybrid) override;

 private:
  wl_display* display_{};
  wl_surface* surface_;
  wl_surface* parent_surface_;
  wl_callback* callback_;
  wl_subsurface* subsurface_{};

  static void on_frame(void* data, wl_callback* callback, uint32_t time);
  static const wl_callback_listener frame_listener;

  std::string flutterAssetsPath_;
  std::optional<int32_t> currentAnimationIndex_;

  std::optional<std::unique_ptr<view_filament_view::Model>> model_;
  std::optional<std::unique_ptr<view_filament_view::Scene>> scene_;
  std::optional<std::unique_ptr<
      std::vector<std::unique_ptr<view_filament_view::Shape>>>>
      shapes_;

  ::filament::Engine* engine_;
  ::filament::gltfio::MaterialProvider* materialProvider_;
  ::filament::gltfio::AssetLoader* assetLoader_;
  std::unique_ptr<filament::gltfio::ResourceLoader> resourceLoader_;

  std::unique_ptr<CustomModelViewer> modelViewer_;

  // std::unique_ptr<IBLProfiler> iblProfiler_;

  std::unique_ptr<view_filament_view::models::glb::GlbLoader> glbLoader_;
  std::unique_ptr<view_filament_view::models::gltf::GltfLoader> gltfLoader_;
  std::unique_ptr<LightManager> lightManager_;
  std::unique_ptr<IndirectLightManager> indirectLightManager_;
  std::unique_ptr<SkyboxManager> skyboxManager_;
  std::unique_ptr<AnimationManager> animationManager_;
  CameraManager* cameraManager_{};
  std::unique_ptr<GroundManager> groundManager_;
  std::unique_ptr<MaterialManager> materialManager_;
  std::unique_ptr<ShapeManager> shapeManager_;

  static void InitializeHandlers(int32_t id);
  void setUpViewer();
  void setUpGround();
  void setUpCamera();
  void setUpSkybox();
  void setUpLight();
  void setUpIndirectLight();
  void setUpLoadingModel();
  void setUpShapes();

  static void OnPlatformChannel(const FlutterPlatformMessage* message,
                                void* userdata);
  static void OnModelState(const FlutterPlatformMessage* message,
                           void* userdata);
  static void OnSceneState(const FlutterPlatformMessage* message,
                           void* userdata);
  static void OnShapeState(const FlutterPlatformMessage* message,
                           void* userdata);
  static void OnRenderer(const FlutterPlatformMessage* message, void* userdata);
};

}  // namespace view_filament_view