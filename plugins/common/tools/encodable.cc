/*
 * Copyright 2023-2024 Toyota Connected North America
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

#include "encodable.h"

#include "../common.h"

namespace plugin_common::Encodable {

void PrintFlutterEncodableMap(const char* name,
                              const flutter::EncodableMap& args) {
  spdlog::warn("[{}]", name);
  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    PrintFlutterEncodableValue(key.c_str(), it.second);
  }
}

void PrintFlutterEncodableList(const char* name,
                               const flutter::EncodableList& list) {
  spdlog::warn("[EncodableList]");
  for (auto& it : list) {
    PrintFlutterEncodableValue(name, it);
  }
}

void PrintFlutterEncodableValue(const char* key,
                                const flutter::EncodableValue& it) {
  if (std::holds_alternative<std::monostate>(it)) {
    spdlog::warn("\t{}: []", key);
  } else if (std::holds_alternative<bool>(it)) {
    auto value = std::get<bool>(it);
    spdlog::warn("\t{}: bool: {}", key, value);
  } else if (std::holds_alternative<int32_t>(it)) {
    auto value = std::get<int32_t>(it);
    spdlog::warn("\t{}: int32_t: {}", key, value);
  } else if (std::holds_alternative<int64_t>(it)) {
    auto value = std::get<double>(it);
    spdlog::warn("\t{}: int64_t: {}", key, value);
  } else if (std::holds_alternative<double>(it)) {
    auto value = std::get<double>(it);
    spdlog::warn("\t{}: double: {}", key, value);
  } else if (std::holds_alternative<std::string>(it)) {
    auto value = std::get<std::string>(it);
    spdlog::warn("\t{}: std::string: [{}]", key, value);
  } else if (std::holds_alternative<std::vector<uint8_t>>(it)) {
    const auto value = std::get<std::vector<uint8_t>>(it);
    spdlog::warn("\t{}: std::vector<uint8_t>", key);
    for (auto const& v : value) {
      spdlog::warn("\t\t{}", v);
    }
  } else if (std::holds_alternative<std::vector<int32_t>>(it)) {
    const auto value = std::get<std::vector<int32_t>>(it);
    spdlog::warn("\t{}: std::vector<int32_t>", key);
    for (auto const& v : value) {
      spdlog::warn("\t\t{}", v);
    }
  } else if (std::holds_alternative<std::vector<int64_t>>(it)) {
    const auto value = std::get<std::vector<int64_t>>(it);
    spdlog::warn("\t{}: std::vector<int64_t>", key);
    for (auto const& v : value) {
      spdlog::warn("\t\t{}", v);
    }
  } else if (std::holds_alternative<std::vector<float>>(it)) {
    const auto value = std::get<std::vector<float>>(it);
    spdlog::warn("\t{}: std::vector<float>", key);
    for (auto const& v : value) {
      spdlog::warn("\t\t{}", v);
    }
  } else if (std::holds_alternative<std::vector<double>>(it)) {
    const auto value = std::get<std::vector<double>>(it);
    spdlog::warn("\t{}: std::vector<double>", key);
    for (auto const& v : value) {
      spdlog::warn("\t\t{}", v);
    }
  } else if (std::holds_alternative<flutter::EncodableList>(it)) {
    spdlog::warn("\t{}: flutter::EncodableList", key);
    const auto val = std::get<flutter::EncodableList>(it);
    PrintFlutterEncodableList(key, val);
  } else if (std::holds_alternative<flutter::EncodableMap>(it)) {
    spdlog::warn("\t{}: flutter::EncodableMap", key);
    const auto val = std::get<flutter::EncodableMap>(it);
    PrintFlutterEncodableMap(key, val);
  } else {
    spdlog::error("\t{}: unknown type", key);
    assert(false);
  }
}
}  // namespace plugin_common::Encodable
