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

#include "material_parameter.h"

#include "logging/logging.h"
#include "utils.h"

namespace plugin_filament_view {

MaterialParameter::MaterialParameter(const std::string& flutter_assets_path,
                                     const flutter::EncodableMap& params)
    : flutterAssetsPath_(flutter_assets_path) {
  SPDLOG_TRACE("++MaterialParameter::MaterialParameter");
  for (auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "name" && std::holds_alternative<std::string>(it.second)) {
      name_ = std::get<std::string>(it.second);
    } else if (key == "type" &&
               std::holds_alternative<std::string>(it.second)) {
      type_ = getTypeForText(std::get<std::string>(it.second));
    } else if (key == "value" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      auto map = std::get<flutter::EncodableMap>(it.second);
      if (type_ == Type::texture) {
        texture_ = std::make_unique<material::texture::Texture>(
            flutterAssetsPath_, map);
      } else {
        Utils::PrintFlutterEncodableMap("Not handled!", map);
      }
    } else if (!it.second.IsNull()) {
      spdlog::debug("[MaterialParameter] Unhandled Parameter");
      Utils::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }
  SPDLOG_TRACE("--MaterialParameter::MaterialParameter");
}

MaterialParameter::~MaterialParameter() {}

void MaterialParameter::Print(const char* tag) {
  spdlog::debug("++++++++");
  spdlog::debug("{} (MaterialParameter)", tag);
  spdlog::debug("\tname: {}", name_);
  spdlog::debug("\ttype: {}", getTextForType(type_));
  if (type_ == Type::texture) {
    if (texture_.has_value()) {
      texture_.value()->Print("\ttexture");
    }
  }
  spdlog::debug("++++++++");
}

const char* MaterialParameter::getTextForType(MaterialParameter::Type type) {
  return (const char*[]){
      kColor, kBool,      kBoolVector, kFloat, kFloatVector,
      kInt,   kIntVector, kMat3,       kMat4,  kTexture,
  }[static_cast<int>(type)];
}

MaterialParameter::Type MaterialParameter::getTypeForText(
    const std::string& type) {
  if (type == kColor) {
    return Type::color;
  } else if (type == kBool) {
    return Type::bool_;
  } else if (type == kBoolVector) {
    return Type::boolVector;
  } else if (type == kFloat) {
    return Type::float_;
  } else if (type == kFloatVector) {
    return Type::floatVector;
  } else if (type == kInt) {
    return Type::int_;
  } else if (type == kIntVector) {
    return Type::intVector;
  } else if (type == kMat3) {
    return Type::mat3;
  } else if (type == kMat4) {
    return Type::mat4;
  } else if (type == kTexture) {
    return Type::texture;
  }
  return Type::int_;
}

}  // namespace plugin_filament_view