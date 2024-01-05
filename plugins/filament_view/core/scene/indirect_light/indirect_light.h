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

#include <memory>
#include <optional>

#include <math/mat3.h>
#include <math/vec3.h>

#include "shell/platform/common/client_wrapper/include/flutter/encodable_value.h"

namespace plugin_filament_view {

class IndirectLight {
 public:
  static constexpr float DEFAULT_LIGHT_INTENSITY = 30'000.0;

  IndirectLight(std::string assetPath, std::string url, float intensity);

  virtual ~IndirectLight() = 0;

  static std::unique_ptr<IndirectLight> Deserialize(
      const flutter::EncodableMap& params);

  [[nodiscard]] const std::string& getAssetPath() const { return assetPath_; };

  [[nodiscard]] const std::string& getUrl() const { return url_; };

  [[nodiscard]] float getIntensity() const { return intensity_; };

 protected:
  std::string assetPath_;
  std::string url_;
  float intensity_;
};

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

class HdrIndirectLight final : public IndirectLight {
 public:
  explicit HdrIndirectLight(std::optional<std::string> assetPath,
                            std::optional<std::string> url,
                            std::optional<float> intensity)
      : IndirectLight(assetPath.has_value() ? std::move(assetPath.value()) : "",
                      url.has_value() ? std::move(url.value()) : "",
                      intensity.has_value()
                          ? intensity.value()
                          : IndirectLight::DEFAULT_LIGHT_INTENSITY) {}

  ~HdrIndirectLight() override = default;
};

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