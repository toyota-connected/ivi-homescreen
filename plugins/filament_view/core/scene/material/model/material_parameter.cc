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

#include <optional>

#include <utility>

#include "plugins/common/common.h"

namespace plugin_filament_view {

MaterialParameter::MaterialParameter(std::string name,
                                     MaterialType type,
                                     MaterialValue value)
    : name_(std::move(name)), type_(type), value_(std::move(value)) {}

std::unique_ptr<MaterialParameter> MaterialParameter::Deserialize(
    const std::string& /* flutter_assets_path */,
    const flutter::EncodableMap& params) {
  SPDLOG_TRACE("++MaterialParameter::Deserialize");
  std::optional<std::string> name;
  std::optional<MaterialType> type;
  std::optional<flutter::EncodableMap> value;

  for (auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "name" && std::holds_alternative<std::string>(it.second)) {
      name = std::get<std::string>(it.second);
    } else if (key == "type" &&
               std::holds_alternative<std::string>(it.second)) {
      type = getTypeForText(std::get<std::string>(it.second));
    } else if (key == "value" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      value = std::get<flutter::EncodableMap>(it.second);
    } else if (!it.second.IsNull()) {
      spdlog::debug("[MaterialParameter] Unhandled Parameter");
      plugin_common::Encodable::PrintFlutterEncodableValue(key.c_str(),
                                                           it.second);
    }
  }

  if (type.has_value()) {
    if (type.value() == MaterialType::TEXTURE) {
      return std::make_unique<MaterialParameter>(
          name.has_value() ? name.value() : "", type.value(),
          Texture::Deserialize(value.value()));
    } else {
      spdlog::error("[MaterialParameter::Deserialize] Unhandled Parameter");
      return {};
    }
  }

  SPDLOG_TRACE("--MaterialParameter::Deserialize");
}

MaterialParameter::~MaterialParameter() = default;

void MaterialParameter::Print(const char* tag) {
  spdlog::debug("++++++++");
  spdlog::debug("{} (MaterialParameter)", tag);
  spdlog::debug("\tname: {}", name_);
  spdlog::debug("\ttype: {}", getTextForType(type_));
  if (type_ == MaterialType::TEXTURE) {
    if (std::holds_alternative<std::unique_ptr<Texture>>(value_)) {
      auto texture = std::get<std::unique_ptr<Texture>>(value_).get();
      if (texture) {
        texture->Print("\ttexture");
      } else {
        spdlog::debug("[MaterialParameter] Texture Empty");
      }
    }
  }
  spdlog::debug("++++++++");
}

const char* MaterialParameter::getTextForType(
    MaterialParameter::MaterialType type) {
  return (const char*[]){
      kColor, kBool,      kBoolVector, kFloat, kFloatVector,
      kInt,   kIntVector, kMat3,       kMat4,  kTexture,
  }[static_cast<int>(type)];
}

MaterialParameter::MaterialType MaterialParameter::getTypeForText(
    const std::string& type) {
  if (type == kColor) {
    return MaterialType::COLOR;
  } else if (type == kBool) {
    return MaterialType::BOOL;
  } else if (type == kBoolVector) {
    return MaterialType::BOOL_VECTOR;
  } else if (type == kFloat) {
    return MaterialType::FLOAT;
  } else if (type == kFloatVector) {
    return MaterialType::FLOAT_VECTOR;
  } else if (type == kInt) {
    return MaterialType::INT;
  } else if (type == kIntVector) {
    return MaterialType::INT_VECTOR;
  } else if (type == kMat3) {
    return MaterialType::MAT3;
  } else if (type == kMat4) {
    return MaterialType::MAT4;
  } else if (type == kTexture) {
    return MaterialType::TEXTURE;
  }
  return MaterialType::INT;
}

}  // namespace plugin_filament_view