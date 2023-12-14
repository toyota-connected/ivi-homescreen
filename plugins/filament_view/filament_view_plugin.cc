
#include "filament_view_plugin.h"

#include <flutter/standard_message_codec.h>

#include <filament/SwapChain.h>

#include "flutter_desktop_view_controller_state.h"
#include "flutter_homescreen.h"
#include "messages.g.h"

#include "logging/logging.h"
#include "utils.h"

class FlutterView;
class Display;

namespace plugin_filament_view {

extern "C" {
extern const uint8_t UBERARCHIVE_PACKAGE[];
extern int UBERARCHIVE_DEFAULT_OFFSET;
extern size_t UBERARCHIVE_DEFAULT_SIZE;
}
#define UBERARCHIVE_DEFAULT_DATA \
  (UBERARCHIVE_PACKAGE + UBERARCHIVE_DEFAULT_OFFSET)

// static
void FilamentViewPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrar* registrar,
    int32_t id,
    std::string viewType,
    int32_t direction,
    double width,
    double height,
    const std::vector<uint8_t>& params,
    std::string assetDirectory,
    FlutterDesktopEngineRef engine) {
  auto plugin = std::make_unique<FilamentViewPlugin>(
      id, std::move(viewType), direction, width, height, params,
      std::move(assetDirectory), engine);

  FilamentViewApi::SetUp(registrar->messenger(), plugin.get(), id);
  ModelStateChannelApi::SetUp(registrar->messenger(), plugin.get(), id);
  SceneStateApi::SetUp(registrar->messenger(), plugin.get(), id);
  ShapeStateApi::SetUp(registrar->messenger(), plugin.get(), id);
  RendererChannelApi::SetUp(registrar->messenger(), plugin.get(), id);

  registrar->AddPlugin(std::move(plugin));
}

FilamentViewPlugin::FilamentViewPlugin(int32_t id,
                                       std::string viewType,
                                       int32_t direction,
                                       double width,
                                       double height,
                                       const std::vector<uint8_t>& params,
                                       std::string assetDirectory,
                                       FlutterDesktopEngineState* state)
    : PlatformView(id, std::move(viewType), direction, width, height),
      callback_(nullptr),
      flutterAssetsPath_(std::move(assetDirectory)) {
  SPDLOG_DEBUG("FilamentViewPlugin: [{}] {}, assetDirectory: {}", GetId(),
               GetViewType(), flutterAssetsPath_);

  assert(state);
  assert(state->view_controller);
  view_ = state->view_controller->view;
  assert(view_);
  display_ = view_->GetDisplay()->GetDisplay();
  assert(display_);
  parent_surface_ = view_->GetWindow()->GetBaseSurface();
  assert(parent_surface_);
  surface_ = wl_compositor_create_surface(view_->GetDisplay()->GetCompositor());
  assert(surface_);
  subsurface_ = wl_subcompositor_get_subsurface(
      view_->GetDisplay()->GetSubCompositor(), surface_, parent_surface_);
  assert(subsurface_);

  native_window_ = {.display = display_,
                    .surface = surface_,
                    .width = static_cast<uint32_t>(width_),
                    .height = static_cast<uint32_t>(height_)};

  // Sync
  // wl_subsurface_set_sync(subsurface_);
  wl_subsurface_set_desync(subsurface_);

  auto& codec = flutter::StandardMessageCodec::GetInstance();
  const auto decoded = codec.DecodeMessage(params.data(), params.size());

  const auto params_ = std::get_if<flutter::EncodableMap>(decoded.get());
  for (auto& it : *params_) {
    if (it.second.IsNull())
      continue;
    auto key = std::get<std::string>(it.first);

    if (key == "model") {
      model_ = std::make_unique<plugin_filament_view::Model>(
          this, flutterAssetsPath_, it.second);
      model_.value()->Print("Model:");
    } else if (key == "scene") {
      scene_ = std::make_unique<plugin_filament_view::Scene>(
          nullptr, flutterAssetsPath_, it.second);
      scene_.value()->Print("Scene:");
    } else if (key == "shapes" &&
               std::holds_alternative<flutter::EncodableList>(it.second)) {
      auto list = std::get<flutter::EncodableList>(it.second);
      for (const auto& it_ : list) {
        if (!it_.IsNull() &&
            std::holds_alternative<flutter::EncodableList>(it_)) {
          auto shape = std::make_unique<plugin_filament_view::Shape>(
              nullptr, flutterAssetsPath_,
              std::get<flutter::EncodableMap>(it_));
          shape->Print("Shape:");
          shapes_.value()->emplace_back(shape.release());
        }
      }
    } else {
      spdlog::debug("[FilamentView] Unhandled Parameter");
      Utils::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }

  engine_ = ::filament::Engine::create(::filament::Engine::Backend::VULKAN);
  swapChain_ = engine_->createSwapChain(
      &native_window_, ::filament::SwapChain::CONFIG_HAS_STENCIL_BUFFER);
  renderer_ = engine_->createRenderer();

  materialProvider_ = ::filament::gltfio::createUbershaderProvider(
      engine_, UBERARCHIVE_DEFAULT_DATA, UBERARCHIVE_DEFAULT_SIZE);
  assetLoader_ =
      ::filament::gltfio::AssetLoader::create({.engine = engine_,
                                               .materials = materialProvider_,
                                               .names = nullptr,
                                               .entities = nullptr,
                                               .defaultNodeName = nullptr});
  resourceLoader_ = std::make_unique<::filament::gltfio::ResourceLoader>(
      ::filament::gltfio::ResourceLoader({.engine = engine_,
                                          .gltfPath = nullptr,
                                          .normalizeSkinningWeights = true}));
  setUpViewer();
  setUpGround();
  setUpCamera();
  setUpSkybox();
  setUpLight();
  setUpIndirectLight();
  setUpLoadingModel();
  setUpShapes();
}

FilamentViewPlugin::~FilamentViewPlugin() {
  Dispose(false);
};

void FilamentViewPlugin::Resize(double width, double height) {
  width_ = static_cast<int32_t>(width);
  height_ = static_cast<int32_t>(height);
  SPDLOG_TRACE("Resize: {} {}", width, height);
}

void FilamentViewPlugin::SetDirection(int32_t direction) {
  direction_ = direction;
  SPDLOG_TRACE("SetDirection: {}", direction_);
}

void FilamentViewPlugin::SetOffset(double left, double top) {
  left_ = static_cast<int32_t>(left);
  top_ = static_cast<int32_t>(top);
  if (subsurface_) {
    SPDLOG_TRACE("SetOffset: left: {}, top: {}", left_, top_);
    if (!callback_) {
      OnFrame(this, callback_, 0);
    }
  }
}

void FilamentViewPlugin::Dispose(bool /* hybrid */) {
  if (renderer_) {
    engine_->destroy(renderer_);
    renderer_ = nullptr;
  }

  if (swapChain_) {
    engine_->destroy(swapChain_);
    swapChain_ = nullptr;
  }

  if (callback_) {
    wl_callback_destroy(callback_);
    callback_ = nullptr;
  }

  if (subsurface_) {
    wl_subsurface_destroy(subsurface_);
    subsurface_ = nullptr;
  }

  if (surface_) {
    wl_surface_destroy(surface_);
    surface_ = nullptr;
  }
}

void FilamentViewPlugin::OnFrame(void* data,
                                 wl_callback* callback,
                                 const uint32_t time) {
  const auto obj = static_cast<FilamentViewPlugin*>(data);

  obj->callback_ = nullptr;

  if (callback) {
    wl_callback_destroy(callback);
  }

  obj->DrawFrame(time);

  // Z-Order
  // wl_subsurface_place_above(obj->subsurface_, obj->parent_surface_);
  wl_subsurface_place_below(obj->subsurface_, obj->parent_surface_);

  obj->callback_ = wl_surface_frame(obj->surface_);
  wl_callback_add_listener(obj->callback_, &FilamentViewPlugin::frame_listener,
                           data);

  wl_subsurface_set_position(obj->subsurface_, obj->left_, obj->top_);

  wl_surface_commit(obj->surface_);
}

const wl_callback_listener FilamentViewPlugin::frame_listener = {.done =
                                                                     OnFrame};

void FilamentViewPlugin::DrawFrame(uint32_t /* time */) const {}

void FilamentViewPlugin::ChangeAnimationByIndex(
    const int32_t index,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeAnimationByName(
    std::string name,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::GetAnimationNames(
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::GetAnimationCount(
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::GetCurrentAnimationIndex(
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::GetAnimationNameByIndex(
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeSkyboxByAsset(
    std::string path,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeSkyboxByUrl(
    std::string url,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeSkyboxByHdrAsset(
    std::string path,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeSkyboxByHdrUrl(
    std::string url,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeSkyboxColor(
    std::string color,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeToTransparentSkybox(
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeLightByKtxAsset(
    std::string path,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeLightByKtxUrl(
    std::string url,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeLightByIndirectLight(
    std::string path,
    double intensity,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeLightByHdrUrl(
    std::string path,
    double intensity,
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::ChangeToDefaultIndirectLight(
    const std::function<void(std::optional<FlutterError> reply)> result) {}

void FilamentViewPlugin::setUpViewer() {
  SPDLOG_DEBUG("[FilamentView] setupViewer");

  modelViewer_ = std::make_unique<CustomModelViewer>(this);

  // surfaceView.setOnTouchListener(modelViewer)
  // surfaceView.setZOrderOnTop(true) // necessary

  glbLoader_ = std::make_unique<models::glb::GlbLoader>(
      nullptr, modelViewer_.get(), flutterAssetsPath_);
  gltfLoader_ = std::make_unique<models::gltf::GltfLoader>(
      nullptr, modelViewer_.get(), flutterAssetsPath_);

  lightManager_ = std::make_unique<LightManager>(modelViewer_.get());
  indirectLightManager_ = std::make_unique<IndirectLightManager>(
      this, modelViewer_.get(), flutterAssetsPath_);  // TODO add iblProfiler
  skyboxManager_ = std::make_unique<SkyboxManager>(
      this, modelViewer_.get(), flutterAssetsPath_);  // TODO add iblProfiler
  animationManager_ = std::make_unique<AnimationManager>(modelViewer_.get());
  cameraManager_ = modelViewer_->getCameraManager();
  groundManager_ =
      std::make_unique<GroundManager>(modelViewer_.get(), flutterAssetsPath_);
  materialManager_ = std::make_unique<MaterialManager>(this, modelViewer_.get(),
                                                       flutterAssetsPath_);
  shapeManager_ = std::make_unique<ShapeManager>(modelViewer_.get(),
                                                 materialManager_.get());
}

void FilamentViewPlugin::setUpGround() {
  // TODO post on platform thread
  if (scene_.has_value() && scene_.value()->getGround()) {
    groundManager_->createGround(scene_.value()->getGround());
  }
}

void FilamentViewPlugin::setUpCamera() {
  if (!scene_.has_value() || !scene_.value()->getCamera())
    return;

  cameraManager_->updateCamera(nullptr);
}

void FilamentViewPlugin::setUpSkybox() {
  // TODO post on platform thread
  auto skybox = scene_.value()->getSkybox();
  if (!skybox) {
    skyboxManager_->setDefaultSkybox();
    // TODO makeSurfaceViewTransparent();
  } else {
    const auto type = skybox->GetType();
    if (type.has_value() && type.value() == 0) {
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

void FilamentViewPlugin::setUpLight() {
  // TODO post on platform thread
  if (scene_.has_value()) {
    auto light = scene_.value()->getLight();
    if (light) {
      lightManager_->changeLight(light);
    } else {
      lightManager_->setDefaultLight();
    }
  } else {
    lightManager_->setDefaultLight();
  }
}

void FilamentViewPlugin::setUpIndirectLight() {
  // TODO post on platform thread
  if (scene_.has_value() && scene_.value()->getIndirectLight()) {
    auto light = scene_.value()->getIndirectLight();
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
void FilamentViewPlugin::setUpLoadingModel() {
  // TODO post on platform thread
#if 0
  val result = loadModel(model)
  if (result != null && model?.fallback != null) {
    if (result is Resource.Error) {
      loadModel(model.fallback)
      setUpAnimation(model.fallback.animation)
    } else {
      setUpAnimation(model.animation)
    }
  } else {
      setUpAnimation(model?.animation)
  }
#endif
}

void FilamentViewPlugin::setUpShapes() {
  // TODO post on platform thread
  if (shapes_.has_value()) {
    shapeManager_->createShapes(*shapes_.value().get());
  }
}

}  // namespace plugin_filament_view