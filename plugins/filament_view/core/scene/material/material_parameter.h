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

#include "core/scene/material/texture/texture.h"
#include "core/scene/material/texture/texture_sampler.h"

namespace plugin_filament_view {
class MaterialParameter {
 public:
  MaterialParameter(const std::string& flutter_assets_path,
                    const flutter::EncodableMap& params);
  ~MaterialParameter();
  void Print(const char* tag);

  // Disallow copy and assign.
  MaterialParameter(const MaterialParameter&) = delete;
  MaterialParameter& operator=(const MaterialParameter&) = delete;

 private:
  enum class Type {
    color,
    bool_,
    boolVector,
    float_,
    floatVector,
    int_,
    intVector,
    mat3,
    mat4,
    texture,
  };

  static constexpr char kColor[] = "COLOR";
  static constexpr char kBool[] = "BOOL";
  static constexpr char kBoolVector[] = "BOOL_VECTOR";
  static constexpr char kFloat[] = "FLOAT";
  static constexpr char kFloatVector[] = "FLOAT_VECTOR";
  static constexpr char kInt[] = "INT";
  static constexpr char kIntVector[] = "INT_VECTOR";
  static constexpr char kMat3[] = "MAT3";
  static constexpr char kMat4[] = "MAT4";
  static constexpr char kTexture[] = "TEXTURE";

  const std::string& flutterAssetsPath_;

  std::string name_;
  Type type_;
  std::optional<std::unique_ptr<material::texture::Texture>> texture_;

  static const char* getTextForType(Type type);
  static Type getTypeForText(const std::string& type);
};
}  // namespace plugin_filament_view