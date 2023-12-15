
#include "filament_scene.h"

#include <flutter/standard_message_codec.h>

#include "logging/logging.h"
#include "scene_controller.h"
#include "utils.h"

namespace plugin_filament_view {

FilamentScene::FilamentScene(PlatformView *platformView,
                             FlutterDesktopEngineState* state,
                             int32_t id,
                             const std::vector<uint8_t>& params,
                             ::filament::Engine* engine,
                             ::filament::gltfio::AssetLoader* assetLoader,
                             ::filament::gltfio::ResourceLoader* resourceLoader,
                             const std::string& flutterAssetsPath) {
  auto& codec = flutter::StandardMessageCodec::GetInstance();
  const auto decoded = codec.DecodeMessage(params.data(), params.size());
  const auto& creationParams =
      std::get_if<flutter::EncodableMap>(decoded.get());

  std::unique_ptr<Model> model;
  std::unique_ptr<Scene> scene;
  std::unique_ptr<std::vector<std::unique_ptr<Shape>>> shapes;

  for (const auto& it : *creationParams) {
    if (it.second.IsNull())
      continue;
    auto key = std::get<std::string>(it.first);

    if (key == "model") {
      model = std::make_unique<plugin_filament_view::Model>(
          this, flutterAssetsPath, it.second);
      model->Print("Model:");
    } else if (key == "scene") {
      scene = std::make_unique<plugin_filament_view::Scene>(
          nullptr, flutterAssetsPath, it.second);
      scene->Print("Scene:");
    } else if (key == "shapes" &&
               std::holds_alternative<flutter::EncodableList>(it.second)) {
      auto list = std::get<flutter::EncodableList>(it.second);
      for (const auto& it_ : list) {
        if (!it_.IsNull() &&
            std::holds_alternative<flutter::EncodableList>(it_)) {
          auto shape = std::make_unique<plugin_filament_view::Shape>(
              nullptr, flutterAssetsPath, std::get<flutter::EncodableMap>(it_));
          shape->Print("Shape:");
          shapes->emplace_back(shape.release());
        }
      }
    } else {
      spdlog::warn("[FilamentView] Unhandled Parameter");
      Utils::PrintFlutterEncodableValue(key.c_str(), it.second);
    }

    sceneController_ = std::make_unique<SceneController>(platformView,
        state, engine, assetLoader, resourceLoader, flutterAssetsPath,
        std::move(model), std::move(scene), std::move(shapes), id);
  }
}

FilamentScene::~FilamentScene() = default;

}  // namespace plugin_filament_view
