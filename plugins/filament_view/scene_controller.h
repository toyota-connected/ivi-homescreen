#pragma once

#include <filament/Engine.h>
#include <gltfio/AssetLoader.h>
#include <gltfio/ResourceLoader.h>

#include "flutter_desktop_engine_state.h"
#include "models/model/animation_manager.h"
#include "models/model/glb/loader/glb_loader.h"
#include "models/model/gltf/loader/gltf_loader.h"
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
  SceneController(PlatformView *platformView,
                  FlutterDesktopEngineState* state,
                  ::filament::Engine* engine,
                  ::filament::gltfio::AssetLoader* assetLoader,
                  ::filament::gltfio::ResourceLoader* resourceLoader,
                  std::string flutterAssetsPath,
                  std::unique_ptr<Model> model,
                  std::unique_ptr<Scene> scene,
                  std::unique_ptr<std::vector<std::unique_ptr<Shape>>> shapes,
                  int32_t id);

 private:
  int32_t id_;
  std::string flutterAssetsPath_;

  std::unique_ptr<Model> model_;
  std::unique_ptr<Scene> scene_;
  std::unique_ptr<std::vector<std::unique_ptr<Shape>>> shapes_;

  std::unique_ptr<CustomModelViewer> modelViewer_;

  // private val choreographer: Choreographer = Choreographer.getInstance()
  // private var modelJob: Job? = null
  // private var glbModelStateJob: Job? = null
  // private var sceneStateJob: Job? = null
  // private var shapeStateJob: Job? = null

  // private val coroutineScope = CoroutineScope(Dispatchers.IO)

  int32_t currentAnimationIndex_;

  // private val surfaceView: SurfaceView = SurfaceView(context)

  std::unique_ptr<models::glb::GlbLoader> glbLoader_;
  std::unique_ptr<models::gltf::GltfLoader> gltfLoader_;
  std::unique_ptr<LightManager> lightManager_;
  std::unique_ptr<IndirectLightManager> indirectLightManager_;
  std::unique_ptr<SkyboxManager> skyboxManager_;
  std::unique_ptr<AnimationManager> animationManager_;
  CameraManager* cameraManager_;
  std::unique_ptr<GroundManager> groundManager_;
  std::unique_ptr<MaterialManager> materialManager_;
  std::unique_ptr<ShapeManager> shapeManager_;

  void setUpViewer();
  void setUpGround();
  void setUpCamera();
  void setUpSkybox();
  void setUpLight();
  void setUpIndirectLight();
  void setUpLoadingModel();
  void setUpShapes();
};
}  // namespace plugin_filament_view
