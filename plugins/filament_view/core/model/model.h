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

#pragma once

#include <functional>
#include <optional>
#include <string>

#include "core/model/animation/animation.h"
#include "core/scene/geometry/position.h"

namespace plugin_filament_view {

class Animation;

class Model {
 public:
  Model(std::string assetPath,
        std::string url,
        Model* fallback,
        float scale,
        ::filament::math::float3* centerPosition,
        Animation* animation);

  virtual ~Model() = default;

  static std::unique_ptr<Model> Deserialize(
      const std::string& flutterAssetsPath,
      const flutter::EncodableValue& params);

  [[nodiscard]] float GetScale() const { return scale_; }

  [[nodiscard]] ::filament::math::float3* GetCenterPosition() const { return center_position_; }

  [[nodiscard]] Model* GetFallback() const { return fallback_; }

  [[nodiscard]] Animation* GetAnimation() const { return animation_; }

  // Disallow copy and assign.
  Model(const Model&) = delete;

  Model& operator=(const Model&) = delete;

 protected:
  std::string assetPath_;
  std::string url_;
  Model* fallback_;
  float scale_;
  ::filament::math::float3* center_position_;
  Animation* animation_;
};

class GlbModel final : public Model {
 public:
  GlbModel(std::string assetPath,
           std::string url,
           Model* fallback,
           float scale,
           ::filament::math::float3* centerPosition,
           Animation* animation);

  ~GlbModel() override = default;

  friend class ModelLoader;

  friend class SceneController;
};

class GltfModel final : public Model {
 public:
  GltfModel(std::string assetPath,
            std::string url,
            std::string pathPrefix,
            std::string pathPostfix,
            Model* fallback,
            float scale,
            ::filament::math::float3* centerPosition,
            Animation* animation);

  ~GltfModel() override = default;

  friend class ModelLoader;

  friend class SceneController;

 private:
  std::string pathPrefix_;
  std::string pathPostfix_;
};

}  // namespace plugin_filament_view
