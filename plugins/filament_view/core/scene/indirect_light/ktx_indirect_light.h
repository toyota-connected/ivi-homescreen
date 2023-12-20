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

namespace plugin_filament_view {

class IndirectLight;

class KtxIndirectLight final : public IndirectLight {
 public:
  explicit KtxIndirectLight(std::optional<std::string> assetPath,
                            std::optional<std::string> url,
                            std::optional<float> intensity)
      : IndirectLight(assetPath.has_value() ? std::move(assetPath.value()) : "",
                      url.has_value() ? std::move(url.value()) : "",
                      intensity.has_value()
                          ? intensity.value()
                          : IndirectLight::DEFAULT_LIGHT_INTENSITY) {}

  ~KtxIndirectLight() override = default;
};
}  // namespace plugin_filament_view