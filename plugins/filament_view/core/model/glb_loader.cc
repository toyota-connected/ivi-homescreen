
#include "glb_loader.h"

#include <sstream>

#include <asio/post.hpp>

#include "../../shell/curl_client/curl_client.h"
#include "core/include/file_utils.h"

#include <utility>

namespace plugin_filament_view {

GlbLoader::GlbLoader(CustomModelViewer* modelViewer,
                     std::string  flutterAssets)
    : modelViewer_(modelViewer),
      flutterAssets_(std::move(flutterAssets)),
      strand_(modelViewer->getStrandContext()) {}

std::future<Resource<std::string>> GlbLoader::loadGlbFromAsset(
    const std::string& path,
    float scale,
    const Position* centerPosition,
    bool isFallback) {
  const auto promise(std::make_shared<std::promise<Resource<std::string>>>());
  auto promise_future(promise->get_future());
  modelViewer_->setModelState(ModelState::LOADING);
  asio::post(strand_, [&, promise, path, scale, centerPosition, isFallback] {
    auto buffer = readBinaryFile(path, flutterAssets_);
    handleFile(buffer, path, scale, centerPosition, isFallback, promise);
  });
  return promise_future;
}

std::future<Resource<std::string>> GlbLoader::loadGlbFromUrl(
    const std::string& url,
    float scale,
    const Position* centerPosition,
    bool isFallback) {
  const auto promise(std::make_shared<std::promise<Resource<std::string>>>());
  auto promise_future(promise->get_future());
  modelViewer_->setModelState(ModelState::LOADING);
  asio::post(strand_, [&, promise, url, scale, centerPosition, isFallback] {
    CurlClient client;
    client.Init(url, {}, {});
    auto buffer = client.RetrieveContentAsVector();
    if (client.GetCode() != CURLE_OK) {
      modelViewer_->setModelState(ModelState::ERROR);
      std::stringstream ss;
      ss << "Couldn't load Glb from " << url;
      promise->set_value(Resource<std::string>::Error(ss.str()));
    }
    handleFile(buffer, url, scale, centerPosition, isFallback, promise);
  });
  return promise_future;
}

void GlbLoader::handleFile(const std::vector<uint8_t>& buffer,
                           const std::string& fileSource,
                           float scale,
                           const Position* centerPosition,
                           bool isFallback,
                           const std::shared_ptr<std::promise<Resource<std::string>>>& promise) {
  if (!buffer.empty()) {
    modelViewer_->getModelLoader()->loadModelGlb(buffer, centerPosition, scale,
                                                 true);
    modelViewer_->setModelState(isFallback ? ModelState::FALLBACK_LOADED
                                           : ModelState::LOADED);
    std::stringstream message;
    message << "Loaded glb model successfully from " << fileSource;
    promise->set_value(Resource<std::string>::Success(message.str()));
  } else {
    modelViewer_->setModelState(ModelState::ERROR);
    std::stringstream message;
    message << "Couldn't load glb model from " << fileSource;
    promise->set_value(Resource<std::string>::Error(message.str()));
  }
}
}  // namespace plugin_filament_view
