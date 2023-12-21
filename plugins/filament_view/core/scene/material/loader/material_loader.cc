
#include "core/scene/material/loader/material_loader.h"

#include <asio/post.hpp>

#include "../../shell/curl_client/curl_client.h"
#include "core/include/file_utils.h"
#include "logging/logging.h"

namespace plugin_filament_view {
MaterialLoader::MaterialLoader(CustomModelViewer* modelViewer,
                               const std::string& assetPath)
    : modelViewer_(modelViewer),
      assetPath_(assetPath),
      engine_(modelViewer->getFilamentEngine()),
      strand_(modelViewer->getStrandContext()) {}

::filament::Material* MaterialLoader::loadMaterialFromAsset(
    const std::string& path) {
  auto buffer = readBinaryFile(path, assetPath_);

  if (!buffer.empty()) {
    auto material = ::filament::Material::Builder()
                        .package(buffer.data(), buffer.size())
                        .build(*engine_);
    return material;
  } else {
    spdlog::error("Could not load material from asset.");
    return nullptr;
  }
}

::filament::Material* MaterialLoader::loadMaterialFromUrl(
    const std::string& url) {
  CurlClient client;
  client.Init(url, {}, {});
  std::vector<uint8_t> buffer = client.RetrieveContentAsVector();
  if (client.GetCode() != CURLE_OK) {
    spdlog::error("Failed to load material from {}", url);
    return nullptr;
  }

  if (!buffer.empty()) {
    auto material = ::filament::Material::Builder()
                        .package(buffer.data(), buffer.size())
                        .build(*engine_);
    return material;
  } else {
    spdlog::error("Could not load material from asset.");
    return nullptr;
  }
}
}  // namespace plugin_filament_view
