
#include "gltf_loader.h"

namespace plugin_filament_view {

GltfLoader::GltfLoader(CustomModelViewer* modelViewer,
                       const std::string& flutterAssets)
    : model_viewer_(modelViewer), flutterAssets_(flutterAssets) {}

std::future<std::string> GltfLoader::loadGltfFromAsset(
    const std::string& path,
    const std::string& prepath,
    const std::string& postpath,
    float scale,
    const Position* centerPosition,
    bool isFallback) {
  const auto promise(std::make_shared<std::promise<std::string>>());
  auto future(promise->get_future());
  promise->set_value("Not implemented yet");
  return future;
}

std::future<std::string> GltfLoader::loadGltfFromUrl(
    const std::string& url,
    float scale,
    const Position* centerPosition,
    bool isFallback) {
  const auto promise(std::make_shared<std::promise<std::string>>());
  auto future(promise->get_future());
  promise->set_value("Not implemented yet");
  return future;
}
}  // namespace plugin_filament_view
