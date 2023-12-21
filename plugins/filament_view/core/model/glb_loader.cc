
#include "glb_loader.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <asio/post.hpp>

#include "../../shell/curl_client/curl_client.h"

namespace plugin_filament_view {

GlbLoader::GlbLoader(CustomModelViewer* modelViewer,
                     const std::string& flutterAssets)
    : model_viewer_(modelViewer),
      flutterAssets_(flutterAssets),
      strand_(modelViewer->getStrandContext()) {}

std::future<std::string> GlbLoader::loadGlbFromAsset(
    const std::string& path,
    float scale,
    const Position* centerPosition,
    bool isFallback) {
  const auto promise(std::make_shared<std::promise<std::string>>());
  auto future(promise->get_future());

  std::filesystem::path asset_path(flutterAssets_);
  asset_path /= path;
  if (path.empty() || !std::filesystem::exists(asset_path)) {
    model_viewer_->setModelState(ModelState::ERROR);
    std::stringstream ss;
    ss << "Glb Path not valid: " << asset_path;
    promise->set_value(ss.str());
    return future;
  }

  model_viewer_->setModelState(ModelState::LOADING);
  asio::post(
      strand_, [&, promise, asset_path, scale, centerPosition, isFallback] {
        std::ifstream stream(asset_path, std::ios::in | std::ios::binary);
        std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(stream)),
                                    std::istreambuf_iterator<char>());

        if (!buffer.empty()) {
          model_viewer_->getModelLoader()->loadModelGlb(buffer, centerPosition,
                                                        scale, true);
          model_viewer_->setModelState(isFallback ? ModelState::FALLBACK_LOADED
                                                  : ModelState::LOADED);
          std::stringstream ss;
          ss << "Loaded glb model successfully from " << asset_path;
          promise->set_value(ss.str());
        } else {
          model_viewer_->setModelState(ModelState::ERROR);
          promise->set_value("Couldn't load glb model from asset");
        }
      });

  return future;
}

std::future<std::string> GlbLoader::loadGlbFromUrl(
    const std::string& url,
    float scale,
    const Position* centerPosition,
    bool isFallback) {
  const auto promise(std::make_shared<std::promise<std::string>>());
  auto future(promise->get_future());

  model_viewer_->setModelState(ModelState::LOADING);

  asio::post(strand_, [&, promise, url, scale, centerPosition, isFallback] {
    CurlClient client;
    client.Init(url, {}, {});
    auto buffer = client.RetrieveContentAsVector();
    if (client.GetCode() != CURLE_OK) {
      model_viewer_->setModelState(ModelState::ERROR);
      std::stringstream ss;
      ss << "Couldn't load Glb from " << url;
      promise->set_value(ss.str());
    }

    if (!buffer.empty()) {
      model_viewer_->getModelLoader()->loadModelGlb(buffer, centerPosition,
                                                    scale, true);
      model_viewer_->setModelState(isFallback ? ModelState::FALLBACK_LOADED
                                              : ModelState::LOADED);
      std::stringstream ss;
      ss << "Loaded glb model successfully from " << url;
      promise->set_value(ss.str());
    } else {
      model_viewer_->setModelState(ModelState::ERROR);
      std::stringstream ss;
      ss << "Couldn't load glb model from " << url;
      promise->set_value(ss.str());
    }
  });

  return future;
}
}  // namespace plugin_filament_view
