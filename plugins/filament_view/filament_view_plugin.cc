
#include "filament_view_plugin.h"

#include <flutter/standard_message_codec.h>

#include <filament/SwapChain.h>

#include "messages.g.h"

#include "logging/logging.h"

#include "filament_scene.h"

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
      flutterAssetsPath_(std::move(assetDirectory)) {
  SPDLOG_DEBUG("FilamentViewPlugin: [{}] {}, assetDirectory: {}", GetId(),
               GetViewType(), flutterAssetsPath_);

  engine_ = ::filament::Engine::create(::filament::Engine::Backend::VULKAN);

  materialProvider_ = ::filament::gltfio::createUbershaderProvider(
      engine_, UBERARCHIVE_DEFAULT_DATA, UBERARCHIVE_DEFAULT_SIZE);
  assetLoader_ =
      ::filament::gltfio::AssetLoader::create({.engine = engine_,
                                               .materials = materialProvider_,
                                               .names = nullptr,
                                               .entities = nullptr,
                                               .defaultNodeName = nullptr});
  // TODO add delete
  resourceLoader_ = new ::filament::gltfio::ResourceLoader(
      {.engine = engine_,
       .gltfPath = nullptr,
       .normalizeSkinningWeights = true});

  filament_scene_ = std::make_unique<FilamentScene>(
      this, state, id, params, engine_, assetLoader_, resourceLoader_,
      flutterAssetsPath_);
}

FilamentViewPlugin::~FilamentViewPlugin() = default;

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
  SPDLOG_TRACE("SetOffset: left: {}, top: {}", left_, top_);
}

void FilamentViewPlugin::Dispose(bool /* hybrid */) {
#if 0
  if (materialProvider_) {
    materialProvider_.reset();
  }
  materialProvider.destroyMaterials() materialProvider.destroy()
      assetLoader.destroy() resourceLoader.destroy() engine.destroy()
#endif
}

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

}  // namespace plugin_filament_view