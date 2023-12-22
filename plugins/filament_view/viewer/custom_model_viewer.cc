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

#include "custom_model_viewer.h"

#include <wayland-client.h>
#include <asio/post.hpp>
#include <utility>

#include "logging/logging.h"
#include "view/flutter_view.h"
#include "wayland/display.h"

class Display;

class FlutterView;

class FilamentViewPlugin;

namespace plugin_filament_view {

CustomModelViewer::CustomModelViewer(PlatformView* platformView,
                                     FlutterDesktopEngineState* state,
                                     std::string flutterAssetsPath)
    : state_(state),
      flutterAssetsPath_(std::move(flutterAssetsPath)),
      left_(platformView->GetOffset().first),
      top_(platformView->GetOffset().second),
      io_context_(std::make_unique<asio::io_context>(ASIO_CONCURRENCY_HINT_1)),
      work_(io_context_->get_executor()),
      strand_(std::make_unique<asio::io_context::strand>(*io_context_)),
      callback_(nullptr),
      fanimator_(nullptr),
      currentModelState_(ModelState::NONE),
      currentSkyboxState_(SceneState::NONE),
      currentLightState_(SceneState::NONE),
      currentGroundState_(SceneState::NONE),
      currentShapesState_(ShapeState::NONE) {
  SPDLOG_TRACE("++CustomModelViewer::CustomModelViewer");
  filament_api_thread_ = std::thread([&]() { io_context_->run(); });
  asio::post(*strand_, [&] {
    filament_api_thread_id_ = pthread_self();
    spdlog::debug("Filament API thread: 0x{:x}", filament_api_thread_id_);
  });

  /* Setup Wayland subsurface */
  auto flutter_view = state->view_controller->view;
  display_ = flutter_view->GetDisplay()->GetDisplay();
  parent_surface_ = flutter_view->GetWindow()->GetBaseSurface();
  surface_ =
      wl_compositor_create_surface(flutter_view->GetDisplay()->GetCompositor());
  subsurface_ = wl_subcompositor_get_subsurface(
      flutter_view->GetDisplay()->GetSubCompositor(), surface_,
      parent_surface_);
  wl_subsurface_set_desync(subsurface_);

  auto f = Initialize(platformView);
  f.wait();

  SPDLOG_TRACE("--CustomModelViewer::CustomModelViewer");
}

CustomModelViewer::~CustomModelViewer() {
  SPDLOG_TRACE("++CustomModelViewer::~CustomModelViewer");

  if (callback_) {
    wl_callback_destroy(callback_);
    callback_ = nullptr;
  }

  cameraManager_->destroyCamera();

  fengine_->destroy(fscene_);
  fengine_->destroy(fview_);
  fengine_->destroy(fskybox_);
  fengine_->destroy(frenderer_);
  fengine_->destroy(fswapChain_);
  ::filament::Engine::destroy(&fengine_);

  modelLoader_.reset();

  if (subsurface_) {
    wl_subsurface_destroy(subsurface_);
    subsurface_ = nullptr;
  }

  if (surface_) {
    wl_surface_destroy(surface_);
    surface_ = nullptr;
  }
  SPDLOG_TRACE("--CustomModelViewer::~CustomModelViewer");
}

std::future<bool> CustomModelViewer::Initialize(PlatformView* platformView) {
  SPDLOG_TRACE("++CustomModelViewer::Initialize");
  auto promise(std::make_shared<std::promise<bool>>());
  auto future(promise->get_future());
  asio::post(*strand_, [&, promise, platformView] {
    auto platform_view_size = platformView->GetSize();
    native_window_ = {
        .display = display_,
        .surface = surface_,
        .width = static_cast<uint32_t>(platform_view_size.first),
        .height = static_cast<uint32_t>(platform_view_size.second)};

    fengine_ = ::filament::Engine::create(::filament::Engine::Backend::VULKAN);
    fswapChain_ = fengine_->createSwapChain(&native_window_);
    frenderer_ = fengine_->createRenderer();

    fview_ = fengine_->createView();
    fscene_ = fengine_->createScene();

    // TODO fview_->setPostProcessingEnabled(false);

    // TODO setupView();

    modelLoader_ = std::make_unique<ModelLoader>(this);

    promise->set_value(true);
  });
  SPDLOG_TRACE("--CustomModelViewer::Initialize");
  return future;
}

void CustomModelViewer::setModelState(ModelState modelState) {
  currentModelState_ = modelState;
  SPDLOG_DEBUG("[FilamentView] setModelState: {}",
               getTextForModelState(currentModelState_));
}

void CustomModelViewer::setGroundState(SceneState sceneState) {
  currentGroundState_ = sceneState;
  SPDLOG_DEBUG("[FilamentView] setGroundState: {}",
               getTextForSceneState(currentGroundState_));
}

void CustomModelViewer::setLightState(SceneState sceneState) {
  currentLightState_ = sceneState;
  SPDLOG_DEBUG("[FilamentView] setLightState: {}",
               getTextForSceneState(currentLightState_));
}

void CustomModelViewer::setSkyboxState(SceneState sceneState) {
  currentSkyboxState_ = sceneState;
  SPDLOG_DEBUG("[FilamentView] setSkyboxState: {}",
               getTextForSceneState(currentSkyboxState_));
}

void CustomModelViewer::destroyIndirectLight() {
  auto scene = fview_->getScene();
  auto indirectLight = scene->getIndirectLight();
  if (indirectLight) {
    fengine_->destroy(indirectLight);
  }
}

void CustomModelViewer::destroySkybox() {
  auto scene = fview_->getScene();
  auto skybox = scene->getSkybox();
  if (skybox) {
    fengine_->destroy(skybox);
  }
}

void CustomModelViewer::setupView() {
  SPDLOG_TRACE("++CustomModelViewer::setupView");
  // on mobile, better use lower quality color buffer
  ::filament::View::RenderQuality renderQuality{};
  renderQuality.hdrColorBuffer = ::filament::View::QualityLevel::MEDIUM;
  fview_->setRenderQuality(renderQuality);

  // dynamic resolution often helps a lot
  ::filament::View::DynamicResolutionOptions dynamicResolutionOptions{};
  dynamicResolutionOptions.enabled = true;
  dynamicResolutionOptions.quality = ::filament::View::QualityLevel::MEDIUM;
  fview_->setDynamicResolutionOptions(dynamicResolutionOptions);

  // MSAA is needed with dynamic resolution MEDIUM
  ::filament::View::MultiSampleAntiAliasingOptions
      multiSampleAntiAliasingOptions{};
  multiSampleAntiAliasingOptions.enabled = true;
  fview_->setMultiSampleAntiAliasingOptions(multiSampleAntiAliasingOptions);

  // FXAA is pretty economical and helps a lot
  fview_->setAntiAliasing(::filament::View::AntiAliasing::FXAA);

  // ambient occlusion is the cheapest effect that adds a lot of quality
  ::filament::View::AmbientOcclusionOptions ambientOcclusionOptions{};
  ambientOcclusionOptions.enabled = true;

  // bloom is pretty expensive but adds a fair amount of realism
  ::filament::View::BloomOptions bloomOptions{};
  bloomOptions.enabled = true;
  fview_->setBloomOptions(bloomOptions);

  SPDLOG_TRACE("--CustomModelViewer::setupView");
}

/**
 * Renders the model and updates the Filament camera.
 *
 * @param frameTimeNanos time in nanoseconds when the frame started being
 * rendered
 */
void CustomModelViewer::DrawFrame(uint32_t time) {
  asio::post(*strand_, [&, time]() {
    modelLoader_->updateScene();

    cameraManager_->lookAtDefaultPosition();

    // Render the scene, unless the renderer wants to skip the frame.
    if (frenderer_->beginFrame(fswapChain_, time)) {
      frenderer_->render(fview_);
      frenderer_->endFrame();
    }
  });
}

void CustomModelViewer::OnFrame(void* data,
                                wl_callback* callback,
                                const uint32_t time) {
  const auto obj = static_cast<CustomModelViewer*>(data);

  obj->callback_ = nullptr;

  if (callback) {
    wl_callback_destroy(callback);
  }

  obj->DrawFrame(time);

  obj->callback_ = wl_surface_frame(obj->surface_);
  wl_callback_add_listener(obj->callback_, &CustomModelViewer::frame_listener,
                           data);

  // Z-Order
  wl_subsurface_place_above(obj->subsurface_, obj->parent_surface_);
  // wl_subsurface_place_below(obj->subsurface_, obj->parent_surface_);
  wl_subsurface_set_position(obj->subsurface_, obj->left_, obj->top_);

  wl_surface_commit(obj->surface_);
}

const wl_callback_listener CustomModelViewer::frame_listener = {.done =
                                                                    OnFrame};

void CustomModelViewer::setOffset(double left, double top) {
  left_ = static_cast<int32_t>(left);
  top_ = static_cast<int32_t>(top);
}

void CustomModelViewer::resize(double width, double height) {
  fview_->setViewport({left_, top_, static_cast<uint32_t>(width),
                       static_cast<uint32_t>(height)});

  cameraManager_->updateCameraOnResize(static_cast<uint32_t>(width),
                                       static_cast<uint32_t>(height));
}

}  // namespace plugin_filament_view
