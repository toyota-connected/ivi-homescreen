
#include "core/scene/material/loader/material_loader.h"

#include "../../shell/curl_client/curl_client.h"
#include "core/include/file_utils.h"

namespace plugin_filament_view {
MaterialLoader::MaterialLoader(CustomModelViewer* modelViewer,
                               const std::string& assetPath)
    : modelViewer_(modelViewer),
      assetPath_(assetPath),
      engine_(modelViewer->getFilamentEngine()),
      strand_(modelViewer->getStrandContext()) {}

Resource<::filament::Material*> MaterialLoader::loadMaterialFromAsset(
    const std::string& path) {
  auto buffer = readBinaryFile(path, assetPath_);

  if (!buffer.empty()) {
    auto material = ::filament::Material::Builder()
                        .package(buffer.data(), buffer.size())
                        .build(*engine_);
    return Resource<::filament::Material*>::Success(material);
  } else {
    return Resource<::filament::Material*>::Error(
        "Could not load material from asset.");
  }
}

Resource<::filament::Material*> MaterialLoader::loadMaterialFromUrl(
    const std::string& url) {
  CurlClient client;
  client.Init(url, {}, {});
  std::vector<uint8_t> buffer = client.RetrieveContentAsVector();
  if (client.GetCode() != CURLE_OK) {
    return Resource<::filament::Material*>::Error(
        "Failed to load material from " + url);
  }

  if (!buffer.empty()) {
    auto material = ::filament::Material::Builder()
                        .package(buffer.data(), buffer.size())
                        .build(*engine_);
    return Resource<::filament::Material*>::Success(material);
  } else {
    return Resource<::filament::Material*>::Error(
        "Could not load material from asset.");
  }
}
}  // namespace plugin_filament_view
