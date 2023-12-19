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

#include "flutter_desktop_engine_state.h"
#include "models/model/animation/animation_manager.h"
#include "models/model/model.h"
#include "models/scene/ground_manager.h"
#include "models/scene/indirect_light/indirect_light_manager.h"
#include "models/scene/light/light_manager.h"
#include "models/scene/material/material_manager.h"
#include "models/scene/scene.h"
#include "models/scene/skybox/skybox_manager.h"
#include "models/shapes/shape.h"
#include "models/shapes/shape_manager.h"
#include "platform_views/platform_view.h"
#include "viewer/custom_model_viewer.h"

namespace plugin_filament_view {

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

  int32_t getAnimationCount() { return animationManager_->getAnimationCount(); }

  void changeAnimation(int32_t animationIndex);

  void changeAnimationByName(std::string animationName);

  std::vector<std::string> getAnimationNames() {
    return animationManager_->getAnimationNames();
  }

  std::optional<int32_t> getCurrentAnimationIndex() {
    return currentAnimationIndex_;
  }

  std::string getAnimationNameByIndex(int32_t index) {
    auto names = animationManager_->getAnimationNames();
    if (index <= names.size()) {
      return names[static_cast<unsigned long>(index)];
    }
    return "";
  }

  CustomModelViewer* getModelViewer() const { return modelViewer_.get(); }

  CameraManager* getCameraManager() const { return cameraManager_.get(); }

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

  // private val coroutineScope = CoroutineScope(Dispatchers.IO)

  std::optional<int32_t> currentAnimationIndex_;

  // private val surfaceView: SurfaceView = SurfaceView(context)

  std::unique_ptr<LightManager> lightManager_;
  std::unique_ptr<IndirectLightManager> indirectLightManager_;
  std::unique_ptr<SkyboxManager> skyboxManager_;
  std::unique_ptr<AnimationManager> animationManager_;
  std::unique_ptr<CameraManager> cameraManager_;
  std::unique_ptr<GroundManager> groundManager_;
  std::unique_ptr<MaterialManager> materialManager_;
  std::unique_ptr<ShapeManager> shapeManager_;

  void setUpViewer(PlatformView* platformView,
                   FlutterDesktopEngineState* state);
  void setUpLoadingModel();
  void setUpCamera();
  void setUpGround();
  void setUpSkybox();
  void setUpLight();
  void setUpIndirectLight();
  void setUpShapes();

  std::string setDefaultCamera();

  std::string loadModel(std::optional<Model*> model);

  void setUpAnimation(std::optional<Animation*> animation);

  void makeSurfaceViewTransparent();
  void makeSurfaceViewNotTransparent();
};
}  // namespace plugin_filament_view
