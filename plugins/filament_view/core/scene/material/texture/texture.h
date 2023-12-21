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

#include "shell/platform/common/client_wrapper/include/flutter/encodable_value.h"

#include "texture_sampler.h"

namespace plugin_filament_view {
class Texture {
 public:
  enum TextureType {
    COLOR,
    NORMAL,
    DATA,
  };

  Texture(TextureType type,
          std::string assetPath,
          std::string url,
          TextureSampler* sampler);

  ~Texture();

  static std::unique_ptr<Texture> Deserialize(
      const flutter::EncodableMap& params);

  void Print(const char* tag);

  // Disallow copy and assign.
  Texture(const Texture&) = delete;
  Texture& operator=(const Texture&) = delete;

  static TextureType getType(const std::string& type);

  static const char* getTextForType(TextureType type);

  friend class TextureLoader;

 private:
  std::string assetPath_;
  std::string url_;
  TextureType type_;
  TextureSampler* sampler_;
};
}  // namespace plugin_filament_view