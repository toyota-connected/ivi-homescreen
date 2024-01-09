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
#include <string>

#include "shell/platform/common/client_wrapper/include/flutter/encodable_value.h"

#include "plugins/common/common.h"

namespace plugin_filament_view {

class Skybox {
 public:
  Skybox(std::string assetPath, std::string url, std::string color);

  virtual ~Skybox() = default;

  static std::unique_ptr<Skybox> Deserialize(
      const flutter::EncodableMap& params);

  [[nodiscard]] const std::string& getAssetPath() const { return assetPath_; }

  [[nodiscard]] const std::string& getUrl() const { return url_; }

  [[nodiscard]] const std::string& getColor() const { return color_; }

 protected:
  std::string assetPath_;
  std::string url_;
  std::string color_;
};

class HdrSkybox final : public Skybox {
 public:
  explicit HdrSkybox(std::optional<std::string> assetPath,
                     std::optional<std::string> url,
                     std::optional<bool> showSun)
      : Skybox(assetPath.has_value() ? std::move(assetPath.value()) : "",
               url.has_value() ? std::move(url.value()) : "",
               ""),
        showSun_(showSun) {}

  ~HdrSkybox() override = default;

  [[nodiscard]] bool getShowSun() const {
    return showSun_.has_value() && showSun_.value();
  };

  friend class SceneController;

 private:
  std::optional<bool> showSun_;
};

class KxtSkybox final : public Skybox {
 public:
  explicit KxtSkybox(std::optional<std::string> assetPath,
                     std::optional<std::string> url)
      : Skybox(assetPath.has_value() ? std::move(assetPath.value()) : "",
               url.has_value() ? std::move(url.value()) : "",
               "") {}

  ~KxtSkybox() override = default;

  friend class SceneController;
};

class ColorSkybox final : public Skybox {
 public:
  explicit ColorSkybox(std::optional<std::string> assetPath,
                       std::optional<std::string> url,
                       std::optional<std::string> color)
      : Skybox(assetPath.has_value() ? std::move(assetPath.value()) : "",
               url.has_value() ? std::move(url.value()) : "",
               color.has_value() ? std::move(color.value()) : "") {}

  ~ColorSkybox() override = default;

  friend class SceneController;
};

}  // namespace plugin_filament_view