#include "glb_loader.h"

#include <sstream>

namespace plugin_filament_view::models::glb {

GlbLoader::GlbLoader(void* context,
                     CustomModelViewer* model_viewer,
                     const std::string& flutter_assets_path)
    : context_(context),
      modelViewer_(model_viewer),
      flutterAssetsPath_(flutter_assets_path) {}

std::future<std::string> GlbLoader::loadGlbFromAsset(
    const std::string& path,
    float scale,
    const Position& centerPosition,
    const bool isFallback) const {
  const auto promise(std::make_shared<std::promise<std::string>>());
  auto future(promise->get_future());

  if (path.empty()) {
    modelViewer_->setModelState(ModelState::error);
    promise->set_value("Asset path is not set");
  }

  modelViewer_->setModelState(ModelState::loading);

  // TODO post to platform task runner
  // TODO bufferResource = readAsset(context, flutterAssetPath_, path);
  // TODO bufferResource.data?.let{modelViewer.modelLoader.loadModelGlb(it,
  // true, centerPosition, scale)} modelViewer

  // TODO error response @withContext Resource.Error(bufferResource.message ?:
  // "Couldn't load glb model from asset")

  modelViewer_->setModelState(isFallback ? ModelState::fallbackLoaded
                                         : ModelState::loaded);
  std::stringstream ss;
  ss << "Loaded glb model successfully from " << path;
  promise->set_value(ss.str());
  return future;
}

std::future<std::string> GlbLoader::loadGlbFromUrl(
    const std::string& url,
    float scale,
    const Position& centerPosition,
    bool isFallback) const {
  const auto promise(std::make_shared<std::promise<std::string>>());
  auto future(promise->get_future());

  // TODO schedule from here
#if 0
      try {
        val buffer = NetworkClient.downloadFile(url) if (buffer != null) {
          modelViewer.modelLoader.loadModelGlb(buffer, true, centerPosition, scale)
          modelViewer.setModelState(
            if (isFallback) ModelState.FALLBACK_LOADED else ModelState.LOADED)
              return @withContext Resource.Success("Loaded glb model successfully from $url")
        }
        else {
          modelViewer.setModelState(ModelState.ERROR) return @withContext Resource.Error("Couldn't load glb model from url: $url")
        }
      } catch (e : Throwable) {
        modelViewer.setModelState(ModelState.ERROR) return @withContext Resource.Error("Couldn't load glb model from url: $url")
      }
    }
  }
#endif  // TODO

  if (url.empty()) {
    modelViewer_->setModelState(ModelState::error);
    promise->set_value("Url is empty");
  }

  modelViewer_->setModelState(ModelState::loading);
  // TODO post to platform runner
  modelViewer_->setModelState(isFallback ? ModelState::fallbackLoaded
                                         : ModelState::loaded);

  std::stringstream ss;
  ss << "Loaded glb model successfully from " << url;
  promise->set_value(ss.str());

  return future;
}

}  // namespace plugin_filament_view::models::glb