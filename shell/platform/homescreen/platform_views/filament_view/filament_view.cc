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

#include "platform_views/filament_view/filament_view.h"

#include <string>
#include <vector>

#include <flutter/standard_message_codec.h>
#include <utils/EntityManager.h>

#include "models/model/model.h"
#include "models/shapes/shape.h"
#include "platform_views/filament_view/models/scene/skybox/skybox_manager.h"
#include "shell/platform_channel.h"
#include "platform_views/platform_view.h"
#include "shell/view/flutter_view.h"
#include "shell/wayland/display.h"

namespace view_filament_view {
static constexpr char kChannelPrefix[] = "io.sourcya.playx.3d.scene";

static constexpr char kKeyModel[] = "model";
static constexpr char kChannelFilamentModelState[] = "model_state_channel";
static constexpr char kKeyScene[] = "scene";
static constexpr char kChannelFilamentSceneState[] = "scene_state";
static constexpr char kKeyShapes[] = "shapes";
static constexpr char kChannelFilamentShapeState[] = "shape_state";

static constexpr char kChannelFilamentRenderChannel[] = "renderer_channel";
static constexpr char kKeyListen[] = "listen";

static constexpr char kMethodChangeAnimationByIndex[] =
    "CHANGE_ANIMATION_BY_INDEX";
static constexpr char kKeyChangeAnimationByIndex[] =
    "CHANGE_ANIMATION_BY_INDEX_KEY";

static constexpr char kMethodChangeAnimationByName[] =
    "CHANGE_ANIMATION_BY_NAME";
static constexpr char kKeyChangeAnimationByName[] =
    "CHANGE_ANIMATION_BY_NAME_KEY";
static constexpr char kMethodGetAnimationNames[] = "GET_ANIMATION_NAMES";

static constexpr char kMethodGetAnimationNameByIndex[] =
    "GET_ANIMATION_NAME_BY_INDEX";
static constexpr char kKeyGetAnimationNameByIndex[] =
    "GET_ANIMATION_NAME_BY_INDEX_KEY";

static constexpr char kMethodGetAnimationCount[] = "GET_ANIMATION_COUNT";

static constexpr char kMethodGetCurrentAnimationIndex[] =
    "GET_CURRENT_ANIMATION_INDEX";

static constexpr char kMethodChangeSkyboxByAsset[] = "CHANGE_SKYBOX_BY_ASSET";
static constexpr char kKeyChangeSkyboxByAsset[] = "CHANGE_SKYBOX_BY_ASSET_KEY";

static constexpr char kMethodChangeSkyboxByUrl[] = "CHANGE_SKYBOX_BY_URL";
static constexpr char kKeyChangeSkyboxByUrl[] = "CHANGE_SKYBOX_BY_URL_KEY";

static constexpr char kMethodChangeSkyboxByHdrAsset[] =
    "CHANGE_SKYBOX_BY_HDR_ASSET";
static constexpr char kKeyChangeSkyboxByHdrAsset[] =
    "CHANGE_SKYBOX_BY_HDR_ASSET_KEY";

static constexpr char kMethodChangeSkyboxByHdrUrl[] =
    "CHANGE_SKYBOX_BY_HDR_URL";
static constexpr char kKeyChangeSkyboxByHdrUrl[] =
    "CHANGE_SKYBOX_BY_HDR_URL_KEY";

static constexpr char kMethodChangeSkyboxColor[] = "CHANGE_SKYBOX_COLOR";
static constexpr char kKeyChangeSkyboxColor[] = "CHANGE_SKYBOX_COLOR_KEY";

static constexpr char kMethodChangeToTransparentSkybox[] =
    "CHANGE_TO_TRANSPARENT_SKYBOX";

static constexpr char kMethodChangeLightByKtxAsset[] = "CHANGE_LIGHT_BY_ASSET";
static constexpr char kKeyChangeLightByKtxAsset[] = "CHANGE_LIGHT_BY_ASSET_KEY";
static constexpr char kKeyChangeLightByKtxAssetIntensity[] =
    "CHANGE_LIGHT_BY_ASSET_INTENSITY_KEY";

static constexpr char kMethodChangeLightByKtxUrl[] = "CHANGE_LIGHT_BY_KTX_URL";
static constexpr char kKeyChangeLightByKtxUrl[] = "CHANGE_LIGHT_BY_KTX_URL_KEY";
static constexpr char kKeyChangeLightByKtxUrlIntensity[] =
    "CHANGE_LIGHT_BY_KTX_URL_INTENSITY_KEY";

static constexpr char kMethodChangeLightByHdrAsset[] =
    "CHANGE_LIGHT_BY_HDR_ASSET";
static constexpr char kKeyChangeLightByHdrAsset[] =
    "CHANGE_LIGHT_BY_HDR_ASSET_KEY";
static constexpr char kKeyChangeLightByHdrAssetIntensity[] =
    "CHANGE_LIGHT_BY_HDR_ASSET_INTENSITY_KEY";

static constexpr char kMethodChangeLightByHdrUrl[] = "CHANGE_LIGHT_BY_HDR_URL";
static constexpr char kKeyChangeLightByHdrUrl[] = "CHANGE_LIGHT_BY_HDR_URL_KEY";
static constexpr char kKeyChangeLightByHdrUrlIntensity[] =
    "CHANGE_LIGHT_BY_HDR_URL_INTENSITY_KEY";

static constexpr char kMethodChangeLightByIndirectLight[] =
    "CHANGE_LIGHT_BY_INDIRECT_LIGHT";
static constexpr char kKeyChangeLightByIndirectLight[] =
    "CHANGE_LIGHT_BY_INDIRECT_LIGHT_KEY";

static constexpr char kMethodChangeToDefaultIndirectLight[] =
    "CHANGE_TO_DEFAULT_LIGHT_INTENSITY";

static constexpr char kMethodChangeLight[] = "CHANGE_LIGHT";
static constexpr char kKeyChangeLight[] = "CHANGE_LIGHT_KEY";
static constexpr char kMethodChangeToDefaultLight[] = "CHANGE_TO_DEFAULT_LIGHT";

static constexpr char kMethodLoadGlbModelFromAssets[] =
    "LOAD_GLB_MODEL_FROM_ASSETS";
static constexpr char kKeyLoadGlbModelFromAssetsPath[] =
    "LOAD_GLB_MODEL_FROM_ASSETS_PATH_KEY";

static constexpr char kMethodLoadGlbModelFromUrl[] = "LOAD_GLB_MODEL_FROM_URL";
static constexpr char kKeyLoadGlbModelFromUrl[] = "LOAD_GLB_MODEL_FROM_URL_KEY";

static constexpr char kMethodLoadGltfModelFromAssets[] =
    "LOAD_GLTF_MODEL_FROM_ASSETS";
static constexpr char kKeyLoadGltfModelFromAssetsPath[] =
    "LOAD_GLTF_MODEL_FROM_ASSETS_PATH_KEY";
static constexpr char kKeyLoadGltfModelFromAssetsPrefixPath[] =
    "LOAD_GLTF_MODEL_FROM_ASSETS_PREFIX_PATH_KEY";
static constexpr char kKeyLoadGltfModelFromAssetsPostfixPath[] =
    "LOAD_GLTF_MODEL_FROM_ASSETS_POSTFIX_PATH_KEY";

static constexpr char kMethodGetCurrentModelState[] = "GET_CURRENT_MODEL_STATE";
static constexpr char kMethodChangeModelScale[] = "CHANGE_MODEL_SCALE";
static constexpr char kKeyChangeModelScale[] = "CHANGE_MODEL_SCALE_KEY";
static constexpr char kMethodChangeModelPosition[] = "CHANGE_MODEL_POSITION";
static constexpr char kKeyChangeModelPosition[] = "CHANGE_MODEL_POSITION_KEY";
static constexpr char kMethodUpdateCamera[] = "UPDATE_CAMERA";
static constexpr char kKeyUpdateCamera[] = "UPDATE_CAMERA_KEY";

static constexpr char kMethodUpdateExposure[] = "UPDATE_EXPOSURE";
static constexpr char kKeyUpdateExposure[] = "UPDATE_EXPOSURE_KEY";

static constexpr char kMethodUpdateProjection[] = "UPDATE_PROJECTION";
static constexpr char kKeyUpdateProjection[] = "UPDATE_PROJECTION_KEY";

static constexpr char kMethodUpdateLensProjection[] = "UPDATE_LENS_PROJECTION";
static constexpr char kKeyUpdateLensProjection[] = "UPDATE_LENS_PROJECTION_KEY";

static constexpr char kMethodUpdateCameraShift[] = "UPDATE_CAMERA_SHIFT";
static constexpr char kKeyUpdateCameraShift[] = "UPDATE_CAMERA_SHIFT_KEY";

static constexpr char kMethodUpdateCameraScaling[] = "UPDATE_CAMERA_SCALING";
static constexpr char kKeyUpdateCameraScaling[] = "UPDATE_CAMERA_SCALING_KEY";

static constexpr char kMethodSetDefaultCamera[] = "SET_DEFAULT_CAMERA";
static constexpr char kMethodLookAtDefaultPosition[] =
    "LOOK_AT_DEFAULT_POSITION";

static constexpr char kMethodLookAtPosition[] = "LOOK_AT_POSITION";
static constexpr char kKeyEyeArray[] = "EYE_ARRAY_KEY";
static constexpr char kKeyTargetArray[] = "TARGET_ARRAY_KEY";
static constexpr char kKeyUpwardArray[] = "UPWARD_ARRAY_KEY";

static constexpr char kMethodGetLookAt[] = "GET_LOOK_AT";
static constexpr char kMethodCameraScroll[] = "CAMERA_SCROLL";

static constexpr char kKeyCameraScrollX[] = "CAMERA_SCROLL_X_KEY";
static constexpr char kKeyCameraScrollY[] = "CAMERA_SCROLL_Y_KEY";
static constexpr char kKeyCameraScrollDelta[] = "CAMERA_SCROLL_DELTA_KEY";

static constexpr char kMethodCameraRayCast[] = "CAMERA_RAYCAST";
static constexpr char kKeyCameraRayCastX[] = "CAMERA_RAYCAST_X_KEY";
static constexpr char kKeyCameraRayCastY[] = "CAMERA_RAYCAST_Y_KEY";

static constexpr char kMethodCameraGrabBegin[] = "CAMERA_GRAB_BEGIN";
static constexpr char kKeyCameraGrabBeginX[] = "CAMERA_GRAB_BEGIN_X_KEY";
static constexpr char kKeyCameraGrabBeginY[] = "CAMERA_GRAB_BEGIN_Y_KEY";
static constexpr char kKeyCameraGrabBeginStrafe[] =
    "CAMERA_GRAB_BEGIN_STRAFE_KEY";

static constexpr char kMethodCameraGrabUpdate[] = "CAMERA_GRAB_UPDATE";
static constexpr char kKeyCameraGrabUpdateX[] = "CAMERA_GRAB_UPDATE_X_KEY";
static constexpr char kKeyCameraGrabUpdateY[] = "CAMERA_GRAB_UPDATE_Y_KEY";
static constexpr char kMethodCameraGrabEnd[] = "CAMERA_GRAB_END";

static constexpr char kMethodUpdateGround[] = "UPDATE_GROUND";
static constexpr char kKeyUpdateGround[] = "UPDATE_GROUND_KEY";

static constexpr char kMethodUpdateGroundMaterial[] = "UPDATE_GROUND_MATERIAL";
static constexpr char kKeyUpdateGroundMaterial[] = "UPDATE_GROUND_MATERIAL_KEY";

static constexpr char kMethodAddShape[] = "ADD_SHAPE";
static constexpr char kKeyAddShape[] = "ADD_SHAPE_KEY";
static constexpr char kMethodRemoveShape[] = "REMOVE_SHAPE";
static constexpr char kKeyRemoveShape[] = "REMOVE_SHAPE_KEY";
static constexpr char kMethodUpdateShape[] = "UPDATE_SHAPE";
static constexpr char kKeyUpdateShape[] = "UPDATE_SHAPE_KEY";
static constexpr char kKeyUpdateShapeId[] = "UPDATE_SHAPE_ID_KEY";
static constexpr char kMethodGetCurrentCreatedShapesIds[] =
    "CREATED_SHAPES_IDS";

FilamentView::FilamentView(int32_t id,
                           std::string viewType,
                           int32_t direction,
                           double width,
                           double height,
                           const std::vector<uint8_t>& params,
                           std::string assetDirectory,
                           FlutterView* view)
    : PlatformView(id, std::move(viewType), direction, width, height),
      callback_(nullptr),
      flutterAssetsPath_(std::move(assetDirectory)) {
  SPDLOG_TRACE("FilamentView: [{}] {}", GetViewType().c_str(), id);

  const auto display = view->GetDisplay();
  display_ = display->GetDisplay();
  surface_ = wl_compositor_create_surface(display->GetCompositor());
  parent_surface_ = view->GetWindow()->GetBaseSurface();
  subsurface_ = wl_subcompositor_get_subsurface(display->GetSubCompositor(),
                                                surface_, parent_surface_);

  //egl_window_ = wl_egl_window_create(surface_, width_, height_);
  //assert(egl_window_);

  //egl_display_ = eglGetDisplay(display->GetDisplay());
  //assert(egl_display_);

  //InitializeEGL();
  //egl_surface_ = eglCreateWindowSurface(
      //egl_display_, egl_config_, reinterpret_cast<EGLNativeWindowType>(egl_window_), nullptr);

  // Sync
  //wl_subsurface_set_sync(subsurface_);
  wl_subsurface_set_desync(subsurface_);

  auto& codec = flutter::StandardMessageCodec::GetInstance();
  const auto decoded = codec.DecodeMessage(params.data(), params.size());

  const auto params_ = std::get_if<flutter::EncodableMap>(decoded.get());
  for (auto& it : *params_) {
    if (it.second.IsNull())
      continue;
    auto key = std::get<std::string>(it.first);

    if (key == kKeyModel) {
      model_ = std::make_unique<view_filament_view::Model>(
          this, flutterAssetsPath_, it.second);
      model_.value()->Print("Model:");
    } else if (key == kKeyScene) {
      scene_ = std::make_unique<view_filament_view::Scene>(
          nullptr, flutterAssetsPath_, it.second);
      scene_.value()->Print("Scene:");
    } else if (key == kKeyShapes &&
               std::holds_alternative<flutter::EncodableList>(it.second)) {
      auto list = std::get<flutter::EncodableList>(it.second);
      for (const auto& it_ : list) {
        if (!it_.IsNull() &&
            std::holds_alternative<flutter::EncodableList>(it_)) {
          auto shape = std::make_unique<view_filament_view::Shape>(
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

  InitializeHandlers(id);

  setUpViewer();
  setUpGround();
  setUpCamera();
  setUpSkybox();
  setUpLight();
  setUpIndirectLight();
  setUpLoadingModel();
  setUpShapes();
}

FilamentView::~FilamentView() = default;

void FilamentView::Resize(const double width, const double height) {
  width_ = static_cast<int32_t>(width);
  height_ = static_cast<int32_t>(height);
  SPDLOG_TRACE("Resize: {} {}", width, height);
}

void FilamentView::SetDirection(const int32_t direction) {
  direction_ = direction;
  SPDLOG_TRACE("SetDirection: {}", direction_);
}

void FilamentView::SetOffset(const double left, const double top) {
  left_ = static_cast<int32_t>(left);
  top_ = static_cast<int32_t>(top);
  if (subsurface_) {
    SPDLOG_TRACE("SetOffset: left: {}, top: {}", left_, top_);
    wl_subsurface_set_position(subsurface_, left_, top_);
    if (!callback_) {
      on_frame(this, callback_, 0);
    }
  }
}

void FilamentView::Dispose(const bool /* hybrid */) {
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

void FilamentView::on_frame(void* data,
                               wl_callback* callback,
                               const uint32_t time) {
  const auto obj = static_cast<FilamentView*>(data);

  obj->callback_ = nullptr;

  if (callback) {
    wl_callback_destroy(callback);
  }

  //TODO obj->DrawFrame(time);

  // Z-Order
  //wl_subsurface_place_above(obj->subsurface_, obj->parent_surface_);
  wl_subsurface_place_below(obj->subsurface_, obj->parent_surface_);

  obj->callback_ = wl_surface_frame(obj->surface_);
  wl_callback_add_listener(obj->callback_, &FilamentView::frame_listener,
                           data);

  wl_subsurface_set_position(obj->subsurface_, obj->left_, obj->top_);

  wl_surface_commit(obj->surface_);
}

const wl_callback_listener FilamentView::frame_listener = {.done = on_frame};

#if 0 //TODO
void FilamentView::OnPlatformChannel(const FlutterPlatformMessage* message,
                                     void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  auto engine = static_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();
  auto arguments = obj->arguments();

  SPDLOG_DEBUG("[PlatformViewFilament::OnPlatformChannel]");
  SPDLOG_DEBUG("\tmethod: {}", method);
  // Utils::PrintFlutterEncodableValue("arguments", *arguments);

  if (method == kMethodChangeAnimationByIndex) {
    /// Updates the current animation by index.
    /// Returns int32_t the updated animation index.
    int32_t change_animation_by_index = 0;
    auto args = std::get_if<flutter::EncodableMap>(obj->arguments());
    for (auto& it : *args) {
      auto key = std::get<std::string>(it.first);
      if (key == kKeyChangeAnimationByIndex &&
          std::holds_alternative<int32_t>(it.second)) {
        change_animation_by_index = std::get<int32_t>(it.second);
      }
    }
    spdlog::debug("{}: {}", kMethodChangeAnimationByIndex,
                  change_animation_by_index);

    auto res = flutter::EncodableValue(change_animation_by_index);
    result = codec.EncodeSuccessEnvelope(&res);

  } else if (method == kMethodChangeAnimationByName) {
    /// param - args
    /// kKeyChangeAnimationByName: String? animationName
    /// Updates the current animation by animation name.
    /// Returns int the updated animation index.
    std::string change_animation_by_name;
    auto args = std::get_if<flutter::EncodableMap>(obj->arguments());
    for (auto& it : *args) {
      auto key = std::get<std::string>(it.first);
      if (key == kKeyChangeAnimationByName &&
          std::holds_alternative<std::string>(it.second)) {
        change_animation_by_name = std::get<std::string>(it.second);
      }
    }
    spdlog::debug("{}: {}", kMethodChangeAnimationByName,
                  change_animation_by_name);

    int32_t animation_index = 0;
    auto res = flutter::EncodableValue(animation_index);
    result = codec.EncodeSuccessEnvelope(&res);

  } else if (method == kMethodGetAnimationNames) {
    /// param - none
    /// Get current model animation names.
    /// Returns String List<Object?> that it's succeeded.
    spdlog::debug("{}: TODO", kMethodGetAnimationNames);

  } else if (method == kMethodGetAnimationCount) {
    /// param - none
    /// Get current model animation count.
    /// Returns int
    spdlog::debug("{}: TODO", kMethodGetAnimationCount);

    int32_t animation_count = 1;
    auto res = flutter::EncodableValue(animation_count);
    result = codec.EncodeSuccessEnvelope(&res);

  } else if (method == kMethodGetCurrentAnimationIndex) {
    /// Get current animation index.
    /// Returns int
    spdlog::debug("{}: TODO", kMethodGetCurrentAnimationIndex);

    int32_t current_animation_index = 1;
    auto res = flutter::EncodableValue(current_animation_index);
    result = codec.EncodeSuccessEnvelope(&res);

  } else if (method == kMethodGetAnimationNameByIndex) {
    /// param - args
    /// kKeyGetAnimationNameByIndex: int? index
    /// Get Animation name by given index.
    /// Returns String
    spdlog::debug("{}: TODO", kMethodGetAnimationNameByIndex);

  } else if (method == kMethodChangeSkyboxByAsset) {
    /// param - args
    /// kKeyChangeSkyboxByAsset: String? path
    /// Updates skybox by ktx file from the assets.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeSkyboxByAsset);

  } else if (method == kMethodChangeSkyboxByUrl) {
    /// param - args
    /// kKeyChangeSkyboxByUrl: String? url
    /// Updates skybox by ktx file from [url].
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeSkyboxByUrl);

  } else if (method == kMethodChangeSkyboxByHdrAsset) {
    /// param - args
    /// kKeyChangeSkyboxByHdrAsset: String? path
    /// Updates skybox by hdr file from the assets.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeSkyboxByHdrAsset);

  } else if (method == kMethodChangeSkyboxByHdrUrl) {
    /// param - args
    /// kKeyChangeSkyboxByHdrUrl: String? url
    /// Updates skybox by hdr file from [url].
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeSkyboxByHdrUrl);

  } else if (method == kMethodChangeSkyboxColor) {
    /// param - args
    /// kKeyChangeSkyboxColor: environmentColor (ascii base 16 hex)
    /// Updates skybox by given color.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeSkyboxColor);

  } else if (method == kMethodChangeToTransparentSkybox) {
    /// param - none
    /// Updates skybox to be transparent.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeToTransparentSkybox);

  } else if (method == kMethodChangeLightByKtxAsset) {
    /// param - args
    /// kKeyChangeLightByKtxAsset: String? path,
    /// kKeyChangeLightByKtxAssetIntensity: double? intensity
    /// Updates scene indirect light by ktx file from assets.
    /// also update scene indirect light [intensity] if provided.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeLightByKtxAsset);

  } else if (method == kMethodChangeLightByKtxUrl) {
    /// param - args
    /// kKeyChangeLightByKtxUrl: String? url,
    /// kKeyChangeLightByKtxUrlIntensity: double? intensity
    /// Updates scene indirect light by ktx file from url.
    /// also update scene indirect light [intensity] if provided.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeLightByKtxUrl);

  } else if (method == kMethodChangeLightByIndirectLight) {
    /// param - args
    /// kKeyChangeLightByHdrAsset: String? path
    /// kKeyChangeLightByHdrAssetIntensity: double? intensity
    /// Updates scene indirect light by HDR file from assets.
    /// also update scene indirect light [intensity] if provided.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeLightByIndirectLight);

  } else if (method == kMethodChangeLightByHdrUrl) {
    /// Updates scene indirect light by HDR file from assets.
    /// also update scene indirect light [intensity] if provided.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeLightByHdrUrl);

  } else if (method == kMethodChangeToDefaultIndirectLight) {
    /// Updates scene indirect light to [DefaultIndirectLight] with default
    /// values of :
    /// * intensity which is 40_000.0.
    /// * radiance bands of 1.0,
    /// * radiance sh of [1,1,1]
    /// * irradiance bands of 1.0,
    /// * irradiance sh of [1,1,1]
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeToDefaultIndirectLight);

  } else if (method == kMethodChangeLight) {
    /// Updates scene direct light .
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeLight);

  } else if (method == kMethodChangeToDefaultLight) {
    /// Updates scene direct light To the default value.
    /// With intensity value of 100_000.0.
    /// and color temperature bands of 6_500.0,
    /// and direction of [0.0, -1.0, 0.0],
    /// and cast shadows true.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeToDefaultLight);

  } else if (method == kMethodLoadGlbModelFromAssets) {
    /// Load glb model from assets and replace current model with it.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodLoadGlbModelFromAssets);

  } else if (method == kMethodLoadGlbModelFromUrl) {
    /// Load glb model from url and replace current model with it.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodLoadGlbModelFromUrl);

  } else if (method == kMethodLoadGltfModelFromAssets) {
    /// Load gltf model from assets and replace current model with it.
    /// If the images path that in the gltf file different from the flutter
    /// asset path, consider adding [prefix] to the images path to be before the
    /// image. Or [postfix] to the images path to be after the image. For
    /// example if in the gltf file, the image path is textures/texture and in
    /// assets the image path is assets/models/textures/texture.png [prefix]
    /// must be 'assets/models/' and [postfix] should be '.png'. Returns String
    /// message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodLoadGltfModelFromAssets);

  } else if (method == kMethodChangeModelScale) {
    /// param - double? scale
    /// Updates current model scale.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeModelScale);

  } else if (method == kMethodChangeModelPosition) {
    /// param - Position
    /// Updates current model center position in the world space.
    /// By taking x,y,z coordinates of [centerPosition] in the world space.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodChangeModelPosition);

  } else if (method == kMethodGetCurrentModelState) {
    /// param - none
    /// Get current model state.
    /// Return String (serialized ModelState)
    spdlog::debug("{}: TODO", kMethodGetCurrentModelState);

  } else if (method == kMethodUpdateCamera) {
    /// param - Camera? camera - flutter::EncodableMap
    /// Replace current camera with a new [camera].
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodUpdateCamera);

  } else if (method == kMethodUpdateExposure) {
    /// param - Exposure? exposure - flutter::EncodableMap
    /// Update the current camera exposure.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodUpdateExposure);

  } else if (method == kMethodUpdateProjection) {
    /// param - Projection? projection - flutter::EncodableMap
    /// Update the current camera projection.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodUpdateProjection);

  } else if (method == kMethodUpdateLensProjection) {
    /// param - LensProjection? lensProjection - flutter::EncodableMap
    /// Update the current camera lens projection.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodUpdateLensProjection);

  } else if (method == kMethodUpdateCameraShift) {
    /// param - arg
    /// kKeyUpdateCameraShift: List<double>? shift
    /// Update the current camera shifting.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodUpdateCameraShift);

  } else if (method == kMethodUpdateCameraScaling) {
    /// param - arg
    /// kKeyUpdateCameraScaling: List<double>? scaling
    /// Update the current camera scaling.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodUpdateCameraScaling);

  } else if (method == kMethodSetDefaultCamera) {
    /// param - none
    /// Update the current camera to it's default settings.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodSetDefaultCamera);

  } else if (method == kMethodLookAtDefaultPosition) {
    /// param - none
    /// Makes the camera looks at its default position.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodLookAtDefaultPosition);

  } else if (method == kMethodGetLookAt) {
    /// param - none
    /// Gets the current positions the camera is looking at.
    /// Returns List of 9 elements. List<Object?>?
    /// From [0:2] elements are eye position.
    /// From [3:5] elements are target position.
    /// From [6:8]  elements are upward position
    spdlog::debug("{}: TODO", kMethodGetLookAt);

  } else if (method == kMethodLookAtPosition) {
    /// param - args
    /// kKeyEyeArray: List<double>? eyePos
    /// kKeyTargetArrayKey: List<double>? targetPos
    /// kKeyUpwardArrayKey: List<double>? upwardPos
    /// Sets the camera's model matrix.
    ///
    ///[eyePos] consists of 3 elements :
    /// eyeX – x-axis position of the camera in world space
    /// eyeY – y-axis position of the camera in world space
    /// eyeZ – z-axis position of the camera in world space
    ///
    ///[targetPos] consists of 3 elements :
    /// centerX – x-axis position of the point in world space the camera is
    /// looking at centerY – y-axis position of the point in world space the
    /// camera is looking at centerZ – z-axis position of the point in world
    /// space the camera is looking at
    ///
    ///[upwardPos] consists of 3 elements :
    /// upX – x-axis coordinate of a unit vector denoting the camera's "up"
    /// direction upY – y-axis coordinate of a unit vector denoting the camera's
    /// "up" direction upZ – z-axis coordinate of a unit vector denoting the
    /// camera's "up" direction Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodLookAtPosition);

  } else if (method == kMethodCameraScroll) {
    /// param - args
    /// kKeyCameraScrollX: num? x
    /// kKeyCameraScrollYKey: num? y
    /// kKeyCameraScrollDeltaKey: double? scrollDelta
    /// In MAP and ORBIT modes, dollys the camera along the viewing direction.
    /// In FREE_FLIGHT mode, adjusts the move speed of the camera.
    ///
    /// [x] X-coordinate for point of interest in viewport space, ignored in
    /// FREE_FLIGHT mode [y] Y-coordinate for point of interest in viewport
    /// space, ignored in FREE_FLIGHT mode [scrolldelta] In MAP and ORBIT modes,
    /// negative means "zoom in", positive means "zoom out"
    ///  In FREE_FLIGHT mode, negative means "slower", positive means "faster"
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodCameraScroll);

  } else if (method == kMethodCameraGrabBegin) {
    /// param - args
    /// kKeyCameraGrabBeginX: num? x
    /// kKeyCameraGrabBeginY: num? y
    /// Updates a grabbing session. This must be called at least once between
    /// [beginCameraGrab] / [endCameraGrab] to dirty the camera.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodCameraGrabBegin);

  } else if (method == kMethodCameraGrabUpdate) {
    /// param - args
    /// kKeyCameraGrabUpdateX: num? x,
    /// kKeyCameraGrabUpdateY: num? y,
    /// kKeyCameraGrabBeginStrafeKey: bool? strafe
    /// Starts a grabbing session (i.e. the user is dragging around in the
    /// viewport).
    /// In MAP mode, this starts a panning session. In ORBIT mode, this starts
    /// either rotating or strafing. In FREE_FLIGHT mode, this starts a nodal
    /// panning session. [x] – X-coordinate for point of interest in viewport
    /// space. [y] – Y-coordinate for point of interest in viewport space.
    /// [strafe] – ORBIT mode only; if true, starts a translation rather than a
    /// rotation. Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodCameraGrabUpdate);

  } else if (method == kMethodCameraGrabEnd) {
    /// param - none
    /// Ends a grabbing session.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodCameraGrabEnd);

  } else if (method == kMethodCameraRayCast) {
    /// param - none
    /// kKeyCameraRayCastX: num? x
    /// kKeyCameraRayCastY: num? y
    /// Given a viewport coordinate, picks a point in the ground plane.
    /// returns List<double>?
    spdlog::debug("{}: TODO", kMethodCameraRayCast);

  } else if (method == kMethodUpdateGround) {
    /// param - none
    /// kKeyUpdateGround: Ground? ground - EncodableMap
    /// Updates current Ground by updating it's width, height, and
    /// whether is below model or not without updating material.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodCameraRayCast);

  } else if (method == kMethodUpdateGroundMaterial) {
    /// param - none
    /// kKeyUpdateGroundMaterial: Material material - EncodableMap
    /// Updates current Ground material.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodUpdateGroundMaterial);

  } else if (method == kMethodAddShape) {
    /// param - none
    /// kKeyAddShape: Shape shape - EncodableMap
    /// Add a shape to the scene.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodAddShape);

  } else if (method == kMethodRemoveShape) {
    /// param - none
    /// kKeyRemoveShape: int id
    /// Removes a shape from the scene by given [id].
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodRemoveShape);

  } else if (method == kMethodUpdateShape) {
    /// param - args
    /// kKeyUpdateShape: Shape shape - EncodableMap
    /// kKeyUpdateShapeId: int id - int32
    /// Updates a shape with given [id] with new [shape].
    /// if no shape was found with the given [id], new shape will be created.
    /// Returns String message that it's succeeded.
    spdlog::debug("{}: TODO", kMethodUpdateShape);

  } else if (method == kMethodGetCurrentCreatedShapesIds) {
    /// param - args
    /// Gets current created shapes ids.
    /// Returns List<int>
    spdlog::debug("{}: TODO", kMethodGetCurrentCreatedShapesIds);

  } else {
    spdlog::error(
        "[PlatformViewFilament::OnPlatformChannel] method {} is unhandled",
        method);
    Utils::PrintFlutterEncodableValue("unhandled", *arguments);
    result = codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  }
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
#endif

void FilamentView::OnModelState(const FlutterPlatformMessage* message,
                                void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  const auto engine = static_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  const auto obj =
      codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();
  const auto arguments = obj->arguments();

  SPDLOG_DEBUG("[FilamentView::OnModelState]");
  SPDLOG_DEBUG("\tmethod: {}", method);
  Utils::PrintFlutterEncodableValue("arguments", *arguments);

  if (method == kKeyListen) {
    result = codec.EncodeSuccessEnvelope();
  } else {
    spdlog::error("[FilamentView::OnModelState] method {} is unhandled",
                  method);
    Utils::PrintFlutterEncodableValue("unhandled", *arguments);
    result = codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  }
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}

void FilamentView::OnSceneState(const FlutterPlatformMessage* message,
                                void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  const auto engine = static_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  const auto obj =
      codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();
  const auto arguments = obj->arguments();

  SPDLOG_DEBUG("[FilamentView::OnSceneState]");
  SPDLOG_DEBUG("\tmethod: {}", method);
  Utils::PrintFlutterEncodableValue("arguments", *arguments);

  if (method == kKeyListen) {
    result = codec.EncodeSuccessEnvelope();
  } else {
    spdlog::error("[FilamentView::OnSceneState] method {} is unhandled",
                  method);
    Utils::PrintFlutterEncodableValue("unhandled", *arguments);
    result = codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  }
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}

void FilamentView::OnShapeState(const FlutterPlatformMessage* message,
                                void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  const auto engine = static_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  const auto obj =
      codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();
  const auto arguments = obj->arguments();

  SPDLOG_DEBUG("[FilamentView::OnShapeState]");
  SPDLOG_DEBUG("\tmethod: {}", method);
  Utils::PrintFlutterEncodableValue("arguments", *arguments);

  if (method == kKeyListen) {
    result = codec.EncodeSuccessEnvelope();
  } else {
    spdlog::error("[FilamentView::OnShapeState] method {} is unhandled",
                  method);
    Utils::PrintFlutterEncodableValue("unhandled", *arguments);
    result = codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  }
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}

void FilamentView::OnRenderer(const FlutterPlatformMessage* message,
                              void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  const auto engine = static_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  const auto obj =
      codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();
  const auto arguments = obj->arguments();

  SPDLOG_DEBUG("[FilamentView::OnRenderer]");
  SPDLOG_DEBUG("\tmethod: {}", method);
  Utils::PrintFlutterEncodableValue("arguments", *arguments);

  if (method == kKeyListen) {
    result = codec.EncodeSuccessEnvelope();
  } else {
    spdlog::error("[FilamentView::OnRenderer] method {} is unhandled", method);
    Utils::PrintFlutterEncodableValue("unhandled", *arguments);
    result = codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  }
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}

void FilamentView::InitializeHandlers(const int32_t id) {
  const auto platform_channel = PlatformChannel::GetInstance();

  std::stringstream ss;
#if 0
  ss << kChannelPrefix << ".channel_" << id;
  platform_channel->RegisterCallback(ss.str().c_str(), OnPlatformChannel);
  ss.str("");
#endif

  ss << kChannelPrefix << "." << kChannelFilamentModelState << "_" << id;
  platform_channel->RegisterCallback(ss.str().c_str(), OnModelState);
  ss.str("");

  ss << kChannelPrefix << "." << kChannelFilamentSceneState << "_" << id;
  platform_channel->RegisterCallback(ss.str().c_str(), OnSceneState);
  ss.str("");

  ss << kChannelPrefix << "." << kChannelFilamentShapeState << "_" << id;
  platform_channel->RegisterCallback(ss.str().c_str(), OnShapeState);
  ss.str("");

  ss << kChannelPrefix << "." << kChannelFilamentRenderChannel << "_" << id;
  platform_channel->RegisterCallback(ss.str().c_str(), OnRenderer);
}

void FilamentView::setUpViewer() {
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

void FilamentView::setUpGround() {
  // TODO post on platform thread
  if (scene_.has_value() && scene_.value()->getGround()) {
    groundManager_->createGround(scene_.value()->getGround());
  }
}

void FilamentView::setUpCamera() {
  if (!scene_.has_value() || !scene_.value()->getCamera())
    return;

  cameraManager_->updateCamera(nullptr);
}

void FilamentView::setUpSkybox() {
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

void FilamentView::setUpLight() {
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

void FilamentView::setUpIndirectLight() {
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
void FilamentView::setUpLoadingModel() {
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

void FilamentView::setUpShapes() {
  // TODO post on platform thread
  if (shapes_.has_value()) {
    shapeManager_->createShapes(*shapes_.value().get());
  }
}

}  // namespace view_filament_view