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

namespace plugin_filament_view::material::texture {
class Texture {
 public:
  Texture(const std::string& flutter_assets_path,
          const flutter::EncodableMap& params);
  void Print(const char* tag);

  // Disallow copy and assign.
  Texture(const Texture&) = delete;
  Texture& operator=(const Texture&) = delete;

  enum Type {
    color,
    normal,
    data,
  };

  static Type getType(const std::string& type);
  static const char* getTextForType(Type type);

 private:
  static constexpr char kTypeColor[] = "COLOR";
  static constexpr char kTypeNormal[] = "NORMAL";
  static constexpr char kTypeData[] = "DATA";

  const std::string& flutterAssetsPath_;

  std::string assetPath_;
  std::string url_;
  Type type_;
  std::optional<std::unique_ptr<TextureSampler>> sampler_;
};
}  // namespace plugin_filament_view::material::texture