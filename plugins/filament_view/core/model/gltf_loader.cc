
#include "gltf_loader.h"

namespace plugin_filament_view {

GltfLoader::GltfLoader(CustomModelViewer* modelViewer,
                       const std::string& flutterAssets)
    : modelViewer_(modelViewer), flutterAssets_(flutterAssets) {}

std::future<Resource<std::string>> GltfLoader::loadGltfFromAsset(
    const std::string& path,
    const std::string& pre_path,
    const std::string& post_path,
    float scale,
    const Position* centerPosition,
    bool isFallback) {
  const auto promise(std::make_shared<std::promise<Resource<std::string>>>());
  auto future(promise->get_future());
  promise->set_value(Resource<std::string>::Error("Not implemented yet"));
  return future;
}

std::future<Resource<std::string>> GltfLoader::loadGltfFromUrl(
    const std::string& url,
    float scale,
    const Position* centerPosition,
    bool isFallback) {
  const auto promise(std::make_shared<std::promise<Resource<std::string>>>());
  auto future(promise->get_future());
  promise->set_value(Resource<std::string>::Error("Not implemented yet"));
  return future;
}
}  // namespace plugin_filament_view
