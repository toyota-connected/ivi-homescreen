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

#include <filament/Engine.h>
#include <gltfio/AssetLoader.h>
#include <gltfio/ResourceLoader.h>

#include "core/include/resource.h"
#include "core/model/animation/animation_manager.h"
#include "core/model/model.h"
#include "core/scene/indirect_light/indirect_light_manager.h"
#include "core/scene/light/light_manager.h"
#include "core/scene/material/material_manager.h"
#include "core/scene/skybox/skybox_manager.h"
#include "core/shapes/shape.h"
#include "core/shapes/shape_manager.h"
#include "core/utils/ibl_profiler.h"
#include "flutter_desktop_engine_state.h"
#include "ground_manager.h"
#include "platform_views/platform_view.h"
#include "scene.h"
#include "viewer/custom_model_viewer.h"

namespace plugin_filament_view {

class Model;

class Scene;

class LightManager;

class IndirectLightManager;

class SkyboxManager;

class Animation;

class AnimationManager;

class CameraManager;

class GroundManager;

class MaterialManager;

class ShapeManager;

class IBLProfiler;

class SceneController {
 public:
  SceneController(PlatformView* platformView,
                  FlutterDesktopEngineState* state,
                  std::string flutterAssetsPath,
                  Model* model,
                  Scene* scene,
                  std::vector<std::unique_ptr<Shape>>* shapes,
                  int32_t id);

  ~SceneController();

  [[nodiscard]] CustomModelViewer* getModelViewer() const {
    return modelViewer_.get();
  }

  void onTouch(int32_t action, double x, double y);

  [[nodiscard]] CameraManager* getCameraManager() const {
    return cameraManager_.get();
  }

 private:
  int32_t id_;
  std::string flutterAssetsPath_;

  Model* model_;
  Scene* scene_;
  std::vector<std::unique_ptr<Shape>>* shapes_;

  std::unique_ptr<CustomModelViewer> modelViewer_;

  // private val choreographer: Choreographer = Choreographer.getInstance()
  // private var modelJob: Job? = null
  // private var glbModelStateJob: Job? = null
  // private var sceneStateJob: Job? = null
  // private var shapeStateJob: Job? = null

  std::optional<int32_t> currentAnimationIndex_;

  std::unique_ptr<plugin_filament_view::IBLProfiler> iblProfiler_;
  std::unique_ptr<plugin_filament_view::LightManager> lightManager_;
  std::unique_ptr<plugin_filament_view::IndirectLightManager>
      indirectLightManager_;
  std::unique_ptr<plugin_filament_view::SkyboxManager> skyboxManager_;
  std::unique_ptr<plugin_filament_view::AnimationManager> animationManager_;
  std::unique_ptr<plugin_filament_view::CameraManager> cameraManager_;
  std::unique_ptr<plugin_filament_view::GroundManager> groundManager_;
  std::unique_ptr<plugin_filament_view::MaterialManager> materialManager_;
  std::unique_ptr<plugin_filament_view::ShapeManager> shapeManager_;

  void setUpViewer(PlatformView* platformView,
                   FlutterDesktopEngineState* state);

  void setUpLoadingModel();

  void setUpCamera();

  void setUpGround();

  std::future<void> setUpIblProfiler();

  void setUpSkybox();

  void setUpLight();

  void setUpIndirectLight();

  void setUpShapes();

  std::string setDefaultCamera();

  Resource<std::string_view> loadModel(Model* model);

  void setUpAnimation(std::optional<Animation*> animation);

  void makeSurfaceViewTransparent();

  void makeSurfaceViewNotTransparent();
};
}  // namespace plugin_filament_view
