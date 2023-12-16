
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
  modelViewer_ = std::make_unique<CustomModelViewer>(platformView, state, flutterAssetsPath_);

  setUpViewer();
  setUpGround();
  setUpCamera();
  setUpSkybox();
  setUpLight();
  setUpIndirectLight();
  setUpLoadingModel();
  setUpShapes();

  SPDLOG_TRACE("--SceneController::SceneController");
}

SceneController::~SceneController() {
  SPDLOG_TRACE("SceneController::~SceneController");
}

void SceneController::setUpViewer() {
  // TODO surfaceView.setOnTouchListener(modelViewer)
  //  surfaceView.setZOrderOnTop(true) // necessary

  glbLoader_ = std::make_unique<models::glb::GlbLoader>(
      nullptr, modelViewer_.get(), flutterAssetsPath_);
  gltfLoader_ = std::make_unique<models::gltf::GltfLoader>(
      nullptr, modelViewer_.get(), flutterAssetsPath_);

  lightManager_ = std::make_unique<LightManager>(modelViewer_.get());
  indirectLightManager_ = std::make_unique<IndirectLightManager>(
      this, modelViewer_.get(), flutterAssetsPath_);
  skyboxManager_ = std::make_unique<SkyboxManager>(this, modelViewer_.get(),
                                                   flutterAssetsPath_);
  animationManager_ = std::make_unique<AnimationManager>(modelViewer_.get());
  cameraManager_ = modelViewer_->getCameraManager();
  groundManager_ =
      std::make_unique<GroundManager>(modelViewer_.get(), flutterAssetsPath_);
  materialManager_ = std::make_unique<MaterialManager>(this, modelViewer_.get(),
                                                       flutterAssetsPath_);
  shapeManager_ = std::make_unique<ShapeManager>(modelViewer_.get(),
                                                 materialManager_.get());
}

void SceneController::setUpGround() {
  if (scene_ && scene_->getGround()) {
    groundManager_->createGround(scene_->getGround());
  }
}

void SceneController::setUpCamera() {
  if (!scene_ || !scene_->getCamera())
    return;

  cameraManager_->updateCamera(scene_->getCamera());
}

void SceneController::setUpSkybox() {
  if (!scene_)
    return;

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
  auto result = loadModel(model_);
  SPDLOG_DEBUG("setUpLoadingModel: [{}]", result);
  if (!result.empty() && model_->GetFallback().has_value()) {
    if (result == "Resource.Error") {
      auto f = model_->GetFallback();
      loadModel(f);
      setUpAnimation(f.value()->GetAnimation());
    } else {
      setUpAnimation(model_->GetAnimation());
    }
  } else {
    setUpAnimation(model_->GetAnimation());
  }
}

void SceneController::setUpShapes() {
  if (shapes_) {
    shapeManager_->createShapes(*shapes_);
  }
}

std::string SceneController::loadModel(std::optional<Model*> model) {
  std::string result = "Success";
  if (!model.has_value())
    return "Error.NoModel";

  auto m = model.value();
  if (m->isGlb()) {
    if (!m->GetAssetPath().empty()) {
      auto f = glbLoader_->loadGlbFromAsset(m->GetAssetPath(), m->GetScale(),
                                            m->GetCenterPosition());
      f.wait();
      result = f.get();
    } else if (!m->GetUrl().empty()) {
      auto f = glbLoader_->loadGlbFromUrl(m->GetUrl(), m->GetScale(),
                                          m->GetCenterPosition());
      f.wait();
      result = f.get();
    }
  } else {
    if (!m->GetAssetPath().empty()) {
      auto f = gltfLoader_->loadGltfFromAsset(m->GetAssetPath(), m->GetScale(),
                                              m->GetCenterPosition());
      f.wait();
      result = f.get();
    } else if (!m->GetUrl().empty()) {
      auto f = gltfLoader_->loadGltfFromUrl(m->GetUrl(), m->GetScale(),
                                            m->GetCenterPosition());
      f.wait();
      result = f.get();
    }
  }
  return result;
}

//TODO Move to model viewer
void SceneController::makeSurfaceViewTransparent() {
  modelViewer_->getView()->setBlendMode(
      ::filament::View::BlendMode::TRANSLUCENT);

  // TODO
  // surfaceView.holder.setFormat(PixelFormat.TRANSLUCENT)

  auto clearOptions = modelViewer_->getRenderer()->getClearOptions();
  clearOptions.clear = true;
  modelViewer_->getRenderer()->setClearOptions(clearOptions);
}

//TODO Move to model viewer
void SceneController::makeSurfaceViewNotTransparent() {
  modelViewer_->getView()->setBlendMode(::filament::View::BlendMode::OPAQUE);

  // TODO surfaceView.setZOrderOnTop(true) // necessary
  // TODO surfaceView.holder.setFormat(PixelFormat.OPAQUE)
}

}  // namespace plugin_filament_view