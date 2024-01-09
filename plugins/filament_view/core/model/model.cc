/*
 * Copyright 2020-2023 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "core/model/model.h"

#include "plugins/common/common.h"

namespace plugin_filament_view {

Model::Model(std::string assetPath,
             std::string url,
             Model* fallback,
             float scale,
             Position* centerPosition,
             Animation* animation)
    : assetPath_(std::move(assetPath)),
      url_(std::move(url)),
      fallback_(fallback),
      scale_(scale),
      center_position_(centerPosition),
      animation_(animation) {}

GlbModel::GlbModel(std::string assetPath,
                   std::string url,
                   Model* fallback,
                   float scale,
                   Position* centerPosition,
                   Animation* animation)
    : Model(std::move(assetPath),
            std::move(url),
            fallback,
            scale,
            centerPosition,
            animation) {}

GltfModel::GltfModel(std::string assetPath,
                     std::string url,
                     std::string pathPrefix,
                     std::string pathPostfix,
                     Model* fallback,
                     float scale,
                     Position* centerPosition,
                     Animation* animation)
    : Model(std::move(assetPath),
            std::move(url),
            fallback,
            scale,
            centerPosition,
            animation),
      pathPrefix_(std::move(pathPrefix)),
      pathPostfix_(std::move(pathPostfix)) {}

std::unique_ptr<Model> Model::Deserialize(
    const std::string& flutterAssetsPath,
    const flutter::EncodableValue& params) {
  SPDLOG_TRACE("++Model::Model");
  std::unique_ptr<Animation> animation;
  std::unique_ptr<Model> fallback;
  std::optional<std::string> assetPath;
  std::optional<std::string> pathPrefix;
  std::optional<std::string> pathPostfix;
  std::optional<std::string> url;
  std::optional<float> scale;
  std::unique_ptr<Position> centerPosition;
  std::unique_ptr<Scene> scene;
  bool is_glb = false;

  for (auto& it : std::get<flutter::EncodableMap>(params)) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "animation" &&
        std::holds_alternative<flutter::EncodableMap>(it.second)) {
      animation = std::make_unique<Animation>(
          flutterAssetsPath, std::get<flutter::EncodableMap>(it.second));
    } else if (key == "assetPath" &&
               std::holds_alternative<std::string>(it.second)) {
      assetPath = std::get<std::string>(it.second);
    } else if (key == "centerPosition" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      centerPosition =
          Position::Deserialize(std::get<flutter::EncodableMap>(it.second));
    } else if (key == "fallback" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      fallback = Deserialize(
          flutterAssetsPath,
          flutter::EncodableValue(std::get<flutter::EncodableMap>(it.second)));
    } else if (key == "isGlb" && std::holds_alternative<bool>(it.second)) {
      is_glb = std::get<bool>(it.second);
    } else if (key == "scale" && std::holds_alternative<double>(it.second)) {
      scale = static_cast<float>(std::get<double>(it.second));
    } else if (key == "url" && std::holds_alternative<std::string>(it.second)) {
      url = std::get<std::string>(it.second);
    } else if (key == "pathPrefix" &&
               std::holds_alternative<std::string>(it.second)) {
      pathPrefix = std::get<std::string>(it.second);
    } else if (key == "pathPostfix" &&
               std::holds_alternative<std::string>(it.second)) {
      pathPostfix = std::get<std::string>(it.second);
    } else if (key == "scene" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      scene = std::make_unique<Scene>(flutterAssetsPath, it.second);
    } else if (!it.second.IsNull()) {
      spdlog::debug("[Model] Unhandled Parameter");
      plugin_common::Encodable::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }

  if (is_glb) {
    return std::move(std::make_unique<plugin_filament_view::GlbModel>(
        assetPath.has_value() ? std::move(assetPath.value()) : "",
        url.has_value() ? std::move(url.value()) : "",
        fallback ? fallback.release() : nullptr,
        scale.has_value() ? scale.value() : 1.0f,
        centerPosition ? centerPosition.release() : nullptr,
        animation ? animation.release() : nullptr));
  } else {
    return std::move(std::make_unique<plugin_filament_view::GltfModel>(
        assetPath.has_value() ? std::move(assetPath.value()) : "",
        url.has_value() ? std::move(url.value()) : "",
        pathPrefix.has_value() ? std::move(pathPrefix.value()) : "",
        pathPostfix.has_value() ? std::move(pathPostfix.value()) : "",
        fallback ? fallback.release() : nullptr,
        scale.has_value() ? scale.value() : 1.0f,
        centerPosition ? centerPosition.release() : nullptr,
        animation ? animation.release() : nullptr));
  }
  SPDLOG_TRACE("--Model::Model");
}
}  // namespace plugin_filament_view
