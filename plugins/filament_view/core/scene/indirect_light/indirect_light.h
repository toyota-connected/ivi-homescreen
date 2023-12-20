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

#include <optional>

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

}  // namespace plugin_filament_view