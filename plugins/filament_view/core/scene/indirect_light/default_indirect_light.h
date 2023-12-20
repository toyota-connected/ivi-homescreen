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

#include <utility>

#include "core/scene/indirect_light/indirect_light.h"
#include "core/scene/indirect_light/indirect_light_manager.h"

namespace plugin_filament_view {

class IndirectLight;
class IndirectLightManager;

class DefaultIndirectLight final : public IndirectLight {
 public:
  explicit DefaultIndirectLight(
      float intensity = IndirectLight::DEFAULT_LIGHT_INTENSITY,
      std::vector<::filament::math::float3> radiance = {{1.0f, 1.0f, 1.0f}},
      std::vector<::filament::math::float3> irradiance = {{1.0f, 1.0f, 1.0f}})
      : IndirectLight("", "", intensity),
        radiance_(std::move(radiance)),
        irradiance_(std::move(irradiance)) {}

  ~DefaultIndirectLight() override = default;

  friend class IndirectLightManager;

 private:
  std::vector<::filament::math::float3> radiance_;
  std::vector<::filament::math::float3> irradiance_;
  std::optional<::filament::math::mat3f> rotation_;
};
}  // namespace plugin_filament_view