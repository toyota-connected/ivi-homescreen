
#include "custom_model_viewer.h"
#include <wayland-client.h>

#include "logging/logging.h"
#include "view/flutter_view.h"
#include "wayland/display.h"

class Display;
class FlutterView;

namespace plugin_filament_view {

CustomModelViewer::CustomModelViewer(
    PlatformView* platformView,
    FlutterDesktopEngineState* state,
    ::filament::Engine* engine,
    ::filament::gltfio::AssetLoader* assetLoader,
    filament::gltfio::ResourceLoader* resourceLoader)
    : state_(state),
      callback_(nullptr),
      animator_(nullptr),
      engine_(engine),
      currentModelState_(ModelState::none),
      currentSkyboxState_(SceneState::none),
      currentLightState_(SceneState::none),
      currentGroundState_(SceneState::none),
      currentShapesState_(ShapeState::none),
      assetLoader_(assetLoader),
      resourceLoader_(resourceLoader) {
  assert(state);
  assert(state->view_controller);
  auto flutter_view = state->view_controller->view;
  assert(flutter_view);
  display_ = flutter_view->GetDisplay()->GetDisplay();
  assert(display_);
  parent_surface_ = flutter_view->GetWindow()->GetBaseSurface();
  assert(parent_surface_);
  surface_ =
      wl_compositor_create_surface(flutter_view->GetDisplay()->GetCompositor());
  assert(surface_);
  subsurface_ = wl_subcompositor_get_subsurface(
      flutter_view->GetDisplay()->GetSubCompositor(), surface_,
      parent_surface_);
  assert(subsurface_);

  // Sync
  // wl_subsurface_set_sync(subsurface_);
  wl_subsurface_set_desync(subsurface_);

  renderer_ = engine_->createRenderer();
  auto platform_view_size = platformView->GetSize();
  native_window_ = {.display = display_,
                    .surface = surface_,
                    .width = static_cast<uint32_t>(platform_view_size.first),
                    .height = static_cast<uint32_t>(platform_view_size.second)};
  swapChain_ = engine_->createSwapChain(&native_window_);

  cameraManager_ = std::make_unique<CameraManager>(this);
  scene_ = engine_->createScene();
  view_ = engine_->createView();
  view_->setScene(scene_);
  renderer_ = engine_->createRenderer();

  setupView();

  modelLoader_ =
      std::make_unique<ModelLoader>(this, assetLoader_, resourceLoader_);

  OnFrame(this, nullptr, 0);
}

CustomModelViewer::~CustomModelViewer() {
  if (callback_) {
    wl_callback_destroy(callback_);
    callback_ = nullptr;
  }

  modelLoader_->destroyModel();

  engine_->destroy(swapChain_);
  engine_->destroy(renderer_);
  engine_->destroy(view_);
  engine_->destroy(scene_);

  cameraManager_->destroyCamera();

  if (subsurface_) {
    wl_subsurface_destroy(subsurface_);
    subsurface_ = nullptr;
  }

  if (surface_) {
    wl_surface_destroy(surface_);
    surface_ = nullptr;
  }
}

void CustomModelViewer::setModelState(models::state::ModelState modelState) {
  currentModelState_ = modelState;
  SPDLOG_DEBUG("[FilamentView] setModelState: {}",
               getTextForModelState(currentModelState_));
}

void CustomModelViewer::setupView() {
  // on mobile, better use lower quality color buffer
  ::filament::View::RenderQuality renderQuality{};
  renderQuality.hdrColorBuffer = ::filament::View::QualityLevel::MEDIUM;
  view_->setRenderQuality(renderQuality);

  // dynamic resolution often helps a lot
  ::filament::View::DynamicResolutionOptions dynamicResolutionOptions{};
  dynamicResolutionOptions.enabled = true;
  dynamicResolutionOptions.quality = ::filament::View::QualityLevel::MEDIUM;
  view_->setDynamicResolutionOptions(dynamicResolutionOptions);

  // MSAA is needed with dynamic resolution MEDIUM
  ::filament::View::MultiSampleAntiAliasingOptions
      multiSampleAntiAliasingOptions{};
  multiSampleAntiAliasingOptions.enabled = true;
  view_->setMultiSampleAntiAliasingOptions(multiSampleAntiAliasingOptions);

  // FXAA is pretty economical and helps a lot
  view_->setAntiAliasing(::filament::View::AntiAliasing::FXAA);

  // ambient occlusion is the cheapest effect that adds a lot of quality
  ::filament::View::AmbientOcclusionOptions ambientOcclusionOptions{};
  ambientOcclusionOptions.enabled = true;

  // bloom is pretty expensive but adds a fair amount of realism
#if 0
  ::filament::View::BloomOptions bloomOptions{};
  bloomOptions.enabled = true;
  view_->setBloomOptions(bloomOptions);
#endif
}

/**
 * Renders the model and updates the Filament camera.
 *
 * @param frameTimeNanos time in nanoseconds when the frame started being
 * rendered
 */
void CustomModelViewer::DrawFrame(uint32_t time) {
  // modelLoader.updateScene()

  cameraManager_->lookAtDefaultPosition();

  // Render the scene, unless the renderer wants to skip the frame.
  if (renderer_->beginFrame(swapChain_, time)) {
    renderer_->render(view_);
    renderer_->endFrame();
    // rendererStateFlow.value=frameTimeNanos;
  }
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

  // Z-Order
  // wl_subsurface_place_above(obj->subsurface_, obj->parent_surface_);
  wl_subsurface_place_below(obj->subsurface_, obj->parent_surface_);

  obj->callback_ = wl_surface_frame(obj->surface_);
  wl_callback_add_listener(obj->callback_, &CustomModelViewer::frame_listener,
                           data);

  // TODO  wl_subsurface_set_position(obj->subsurface_, obj->left_, obj->top_);

  wl_surface_commit(obj->surface_);
}

const wl_callback_listener CustomModelViewer::frame_listener = {.done =
                                                                    OnFrame};
}  // namespace plugin_filament_view
