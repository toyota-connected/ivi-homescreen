
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

extern "C" {
extern const uint8_t UBERARCHIVE_PACKAGE[];
extern int UBERARCHIVE_DEFAULT_OFFSET;
extern size_t UBERARCHIVE_DEFAULT_SIZE;
}
#define UBERARCHIVE_DEFAULT_DATA \
  (UBERARCHIVE_PACKAGE + UBERARCHIVE_DEFAULT_OFFSET)

namespace plugin_filament_view {

CustomModelViewer::CustomModelViewer(PlatformView* platformView,
                                     FlutterDesktopEngineState* state,
                                     std::string flutterAssetsPath)
    : state_(state),
      flutterAssetsPath_(std::move(flutterAssetsPath)),
      io_context_(std::make_unique<asio::io_context>(ASIO_CONCURRENCY_HINT_1)),
      work_(io_context_->get_executor()),
      strand_(std::make_unique<asio::io_context::strand>(*io_context_)),
      callback_(nullptr),
      animator_(nullptr),
      currentModelState_(ModelState::none),
      currentSkyboxState_(SceneState::none),
      currentLightState_(SceneState::none),
      currentGroundState_(SceneState::none),
      currentShapesState_(ShapeState::none) {
  SPDLOG_TRACE("++CustomModelViewer::CustomModelViewer");
  filament_api_thread_ = std::thread([&]() { io_context_->run(); });
  asio::post(*strand_, [&] {
    filament_api_thread_id_ = pthread_self();
    spdlog::debug("Filament API thread: 0x{:x}", filament_api_thread_id_);
  });

  /* Setup Wayland variables */
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

  auto f = Initialize(platformView);
  f.wait();

  OnFrame(this, nullptr, 0);
  SPDLOG_TRACE("--CustomModelViewer::CustomModelViewer");
}

CustomModelViewer::~CustomModelViewer() {
  SPDLOG_TRACE("++CustomModelViewer::~CustomModelViewer");

  if (callback_) {
    wl_callback_destroy(callback_);
    callback_ = nullptr;
  }

  delete resourceLoader_;
  resourceLoader_ = nullptr;

  if (assetLoader_) {
    ::filament::gltfio::AssetLoader::destroy(&assetLoader_);
  }

  // modelLoader_.reset();

  if (swapChain_) {
    engine_->destroy(swapChain_);
  }
  if (renderer_) {
    engine_->destroy(renderer_);
  }
  if (view_) {
    engine_->destroy(view_);
  }
  if (scene_) {
    engine_->destroy(scene_);
  }

  cameraManager_->destroyCamera();

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

    engine_ = ::filament::Engine::create(::filament::Engine::Backend::VULKAN);

    materialProvider_ = ::filament::gltfio::createUbershaderProvider(
        engine_, UBERARCHIVE_DEFAULT_DATA, UBERARCHIVE_DEFAULT_SIZE);
    SPDLOG_DEBUG("UbershaderProvider MaterialsCount: {}", materialProvider_->getMaterialsCount());

    ::filament::gltfio::AssetConfiguration assetConfiguration{};
    assetConfiguration.engine = engine_;
    assetConfiguration.materials = materialProvider_;
    assetLoader_ = ::filament::gltfio::AssetLoader::create(assetConfiguration);

    ::filament::gltfio::ResourceConfiguration resourceConfiguration{};
    resourceConfiguration.engine = engine_;
    resourceConfiguration.normalizeSkinningWeights = true;
    resourceLoader_ =
        new ::filament::gltfio::ResourceLoader(resourceConfiguration);

    modelLoader_ = std::make_unique<ModelLoader>(
        this, engine_, assetLoader_, resourceLoader_, strand_.get());

    // Sync
    // wl_subsurface_set_sync(subsurface_);
    wl_subsurface_set_desync(subsurface_);

    renderer_ = engine_->createRenderer();

    auto platform_view_size = platformView->GetSize();
    native_window_ = {
        .display = display_,
        .surface = surface_,
        .width = static_cast<uint32_t>(platform_view_size.first),
        .height = static_cast<uint32_t>(platform_view_size.second)};
    swapChain_ = engine_->createSwapChain(&native_window_);

    cameraManager_ = std::make_unique<CameraManager>(this);
    scene_ = engine_->createScene();
    view_ = engine_->createView();
    view_->setScene(scene_);

    setupView();

    promise->set_value(true);
  });
  SPDLOG_TRACE("--CustomModelViewer::Initialize");
  return future;
}

void CustomModelViewer::setModelState(models::state::ModelState modelState) {
  currentModelState_ = modelState;
  SPDLOG_DEBUG("[FilamentView] setModelState: {}",
               getTextForModelState(currentModelState_));
}

void CustomModelViewer::setupView() {
  SPDLOG_TRACE("++CustomModelViewer::setupView");
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
  ::filament::View::BloomOptions bloomOptions{};
  bloomOptions.enabled = true;
  view_->setBloomOptions(bloomOptions);

  SPDLOG_TRACE("--CustomModelViewer::setupView");
}

/**
 * Renders the model and updates the Filament camera.
 *
 * @param frameTimeNanos time in nanoseconds when the frame started being
 * rendered
 */
void CustomModelViewer::DrawFrame(uint32_t time) {
  if (filament_api_thread_id_ != pthread_self()) {
    asio::post(*strand_, [&]() {

      // modelLoader.updateScene()

      cameraManager_->lookAtDefaultPosition();

      // Render the scene, unless the renderer wants to skip the frame.
      if (renderer_->beginFrame(swapChain_, time)) {
        renderer_->render(view_);
        renderer_->endFrame();
        // rendererStateFlow.value=frameTimeNanos;
      }
    });
  } else {
    spdlog::debug("[DrawFrame]");

    // modelLoader.updateScene()

    cameraManager_->lookAtDefaultPosition();

    // Render the scene, unless the renderer wants to skip the frame.
    if (renderer_->beginFrame(swapChain_, time)) {
      renderer_->render(view_);
      renderer_->endFrame();
      // rendererStateFlow.value=frameTimeNanos;
    }
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
