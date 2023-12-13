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

#include "encodable_value.h"

#include "platform_views/filament_view/models/scene/camera/camera.h"

#include "ground.h"
#include "platform_views/filament_view/models/scene/indirect_light/indirect_light.h"
#include "platform_views/filament_view/models/scene/light/light.h"
#include "platform_views/filament_view/models/scene/skybox/skybox.h"

namespace view_filament_view {
class Scene {
 public:
  Scene(void* parent,
        const std::string& flutter_assets_path,
        const flutter::EncodableValue& val);
  ~Scene() = default;
  void Print(const char* tag) const;
  [[nodiscard]] Ground* getGround() const {
    return ground_.has_value() ? ground_.value().get() : nullptr;
  }
  [[nodiscard]] Camera* getCamera() const {
    return camera_.has_value() ? camera_.value().get() : nullptr;
  }
  [[nodiscard]] Skybox* getSkybox() const {
    return skybox_.has_value() ? skybox_.value().get() : nullptr;
  }
  [[nodiscard]] Light* getLight() const {
    return light_.has_value() ? light_.value().get() : nullptr;
  }
  [[nodiscard]] IndirectLight* getIndirectLight() const {
    return indirect_light_.has_value() ? indirect_light_.value().get()
                                       : nullptr;
  }

  FML_DISALLOW_COPY_AND_ASSIGN(Scene);

 private:
  void* parent_;
  const std::string& flutterAssetsPath_;

  std::optional<std::unique_ptr<Skybox>> skybox_;
  std::optional<std::unique_ptr<IndirectLight>> indirect_light_;
  std::optional<std::unique_ptr<Light>> light_;
  std::optional<std::unique_ptr<Camera>> camera_;
  std::optional<std::unique_ptr<Ground>> ground_;
};
}  // namespace view_filament_view
