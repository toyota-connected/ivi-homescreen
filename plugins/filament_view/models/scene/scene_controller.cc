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

#include "scene_controller.h"

#include <utility>

#include "logging/logging.h"

namespace plugin_filament_view {

SceneController::SceneController(PlatformView* platformView,
                                 FlutterDesktopEngineState* state,
                                 std::string flutterAssetsPath,
                                 Model* model,
                                 Scene* scene,
                                 std::vector<std::unique_ptr<Shape>>* shapes,
                                 int32_t id)
    : id_(id),
      flutterAssetsPath_(std::move(flutterAssetsPath)),
      scene_(scene),
      model_(model),
      shapes_(shapes) {
  SPDLOG_TRACE("++SceneController::SceneController");
  setUpViewer(platformView, state);
  setUpLoadingModel();
  setUpCamera();
  setUpGround();
  setUpSkybox();
  setUpLight();
  setUpIndirectLight();
  setUpShapes();

  modelViewer_->setInitialized();

  SPDLOG_TRACE("--SceneController::SceneController");
}

SceneController::~SceneController() {
  SPDLOG_TRACE("SceneController::~SceneController");
}

void SceneController::setUpViewer(PlatformView* platformView,
                                  FlutterDesktopEngineState* state) {
  modelViewer_ = std::make_unique<CustomModelViewer>(platformView, state,
                                                     flutterAssetsPath_);
  // TODO surfaceView.setOnTouchListener(modelViewer)
  //  surfaceView.setZOrderOnTop(true) // necessary

  auto view = modelViewer_->getFilamentView();
  auto scene = modelViewer_->getFilamentScene();

  auto size = platformView->GetSize();
  view->setViewport({0, 0, static_cast<uint32_t>(size.first),
                     static_cast<uint32_t>(size.second)});

  view->setScene(scene);
  view->setPostProcessingEnabled(false);
}

void SceneController::setUpGround() {
  groundManager_ = std::make_unique<GroundManager>(modelViewer_.get(), nullptr);
  auto f = groundManager_->createGround();
  f.wait();
}

void SceneController::setUpCamera() {
  // we own the camera manager
  cameraManager_ = std::make_unique<CameraManager>(modelViewer_.get());
  // update model viewer
  modelViewer_->setCameraManager(cameraManager_.get());

  auto camera = scene_->getCamera();
  if (!camera) {
    return;
  }

  auto f = cameraManager_->updateCamera(camera);
  f.wait();
}

void SceneController::setUpSkybox() {
  skyboxManager_ = std::make_unique<SkyboxManager>(this, modelViewer_.get(),
                                                   flutterAssetsPath_);

  auto skybox = scene_->getSkybox();
  if (!skybox) {
    skyboxManager_->setDefaultSkybox();
    makeSurfaceViewTransparent();
  } else {
    const auto type = skybox->GetType();
    if (type.has_value()) {
      SPDLOG_DEBUG("skybox type: {}", type.value());
    }
  }

#if 0
  } else {
    when (skybox) {
      is KtxSkybox -> {
        if (!skybox.assetPath.isNullOrEmpty()) {
          skyboxManger.setSkyboxFromKTXAsset(skybox.assetPath)
        } else if (!skybox.url.isNullOrEmpty()) {
        skyboxManger.setSkyboxFromKTXUrl(skybox.url)
      }
    }
    is HdrSkybox -> {
      if (!skybox.assetPath.isNullOrEmpty()) {
        val shouldUpdateLight = skybox.assetPath == scene?.indirectLight?.assetPath
        skyboxManger.setSkyboxFromHdrAsset(
          skybox.assetPath,
          skybox.showSun ?: false,
          shouldUpdateLight,
          scene?.indirectLight?.intensity
        )
      } else if (!skybox.url.isNullOrEmpty()) {
        val shouldUpdateLight = skybox.url == scene?.indirectLight?.url
        skyboxManger.setSkyboxFromHdrUrl(
          skybox.url,
          skybox.showSun ?: false,
          shouldUpdateLight,
          scene?.indirectLight?.intensity
          )
      }
    }
    is ColoredSkybox -> {
      if (skybox.color != null) {
        skyboxManger.setSkyboxFromColor(skybox.color)
      }
    }

    }
  }
#endif
}

void SceneController::setUpLight() {
  lightManager_ = std::make_unique<LightManager>(modelViewer_.get());

  if (scene_) {
    auto light = scene_->getLight();
    if (light) {
      lightManager_->changeLight(light);
    } else {
      lightManager_->setDefaultLight();
    }
  } else {
    lightManager_->setDefaultLight();
  }
}

void SceneController::setUpIndirectLight() {
  indirectLightManager_ = std::make_unique<IndirectLightManager>(
      this, modelViewer_.get(), flutterAssetsPath_);

  if (scene_ && scene_->getIndirectLight()) {
    auto light = scene_->getIndirectLight();
    if (!light) {
      indirectLightManager_->setDefaultIndirectLight(light);
    } else {
      // TODO
    }
  }

#if 0
      when (light) {
        is KtxIndirectLight -> {
          if (!light.assetPath.isNullOrEmpty()) {
            indirectLightManger.setIndirectLightFromKtxAsset(
                light.assetPath, light.intensity
            )
          } else if (!light.url.isNullOrEmpty()) {
            indirectLightManger.setIndirectLightFromKtxUrl(light.url, light.intensity)
          }
        }
        is HdrIndirectLight -> {
          if (!light.assetPath.isNullOrEmpty()) {
            val shouldUpdateLight = light.assetPath != scene?.skybox?.assetPath

            if (shouldUpdateLight) {
              indirectLightManger.setIndirectLightFromHdrAsset(
                  light.assetPath, light.intensity
              )
            }

          } else if (!light.url.isNullOrEmpty()) {
            val shouldUpdateLight = light.url != scene?.skybox?.url
                                                                 if (shouldUpdateLight) {
              indirectLightManger.setIndirectLightFromHdrUrl(light.url, light.intensity)
            }
          }
        }
        is DefaultIndirectLight ->{
          indirectLightManger.setIndirectLight(light)
        }
        else -> {
          indirectLightManger.setDefaultIndirectLight()
        }
      }
    }
#endif
}

void SceneController::setUpAnimation(std::optional<Animation*> animation) {
  if (animation.has_value()) {
    auto a = animation.value();
    if (a->GetAutoPlay()) {
      if (a->GetIndex().has_value()) {
        currentAnimationIndex_ = a->GetIndex();
      } else if (!a->GetName().empty()) {
        currentAnimationIndex_ =
            animationManager_->getAnimationIndexByName(a->GetName());
      }
    }
  } else {
    currentAnimationIndex_ = std::nullopt;
  }
}

void SceneController::setUpLoadingModel() {
  SPDLOG_TRACE("++SceneController::setUpLoadingModel");
  animationManager_ = std::make_unique<AnimationManager>(modelViewer_.get());

  auto result = modelViewer_->loadModel(model_);
  SPDLOG_DEBUG("loadModel: {}", result);
  if (!result.empty() && model_->GetFallback().has_value()) {
    if (result == "Resource.Error") {
      auto f = model_->GetFallback();
      if (f.has_value()) {
        result = modelViewer_->loadModel(f.value());
        SPDLOG_DEBUG("Fallback loadModel: {}", result);
        setUpAnimation(f.value()->GetAnimation());
      } else {
        result = "Error.FallbackLoadFailed";
      }
    } else {
      setUpAnimation(model_->GetAnimation());
    }
  } else {
    setUpAnimation(model_->GetAnimation());
  }
  SPDLOG_TRACE("--SceneController::setUpLoadingModel");
}

void SceneController::setUpShapes() {
  materialManager_ = std::make_unique<MaterialManager>(this, modelViewer_.get(),
                                                       flutterAssetsPath_);
  shapeManager_ = std::make_unique<ShapeManager>(modelViewer_.get(),
                                                 materialManager_.get());
  if (shapes_) {
    shapeManager_->createShapes(*shapes_);
  }
}

std::string SceneController::setDefaultCamera() {
  cameraManager_->setDefaultCamera();
  return "Default camera updated successfully";
}

std::string SceneController::loadModel(std::optional<Model*> model) {
  if (!model.has_value())
    return "Error.NoModel";

  return modelViewer_->loadModel(model.value());
}

// TODO Move to model viewer
void SceneController::makeSurfaceViewTransparent() {
  modelViewer_->getFilamentView()->setBlendMode(
      ::filament::View::BlendMode::TRANSLUCENT);

  // TODO
  // surfaceView.holder.setFormat(PixelFormat.TRANSLUCENT)

  auto clearOptions = modelViewer_->getFilamentRenderer()->getClearOptions();
  clearOptions.clear = true;
  modelViewer_->getFilamentRenderer()->setClearOptions(clearOptions);
}

// TODO Move to model viewer
void SceneController::makeSurfaceViewNotTransparent() {
  modelViewer_->getFilamentView()->setBlendMode(
      ::filament::View::BlendMode::OPAQUE);

  // TODO surfaceView.setZOrderOnTop(true) // necessary
  // TODO surfaceView.holder.setFormat(PixelFormat.OPAQUE)
}

}  // namespace plugin_filament_view