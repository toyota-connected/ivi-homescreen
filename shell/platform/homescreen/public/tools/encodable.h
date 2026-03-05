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

#ifndef PLUGINS_COMMON_TOOLS_ENCODABLE_VALUES_H_
#define PLUGINS_COMMON_TOOLS_ENCODABLE_VALUES_H_

#include <flutter/encodable_value.h>

namespace plugin_common::Encodable {

/**
 * @brief Prints flutter::EncodableMap
 * @return void
 * @relation
 * internal
 */
[[maybe_unused]] void PrintFlutterEncodableMap(
    const char* name,
    const flutter::EncodableMap& args);

/**
 * @brief Prints flutter::EncodableList
 * @return void
 * @relation
 * internal
 */
[[maybe_unused]] void PrintFlutterEncodableList(
    const char* name,
    const flutter::EncodableList& list);

/**
 * @brief Prints flutter::EncodableValue
 * @return void
 * @relation
 * internal
 */
[[maybe_unused]] void PrintFlutterEncodableValue(
    const char* key,
    const flutter::EncodableValue& it);
}  // namespace plugin_common::Encodable

#endif  // PLUGINS_COMMON_TOOLS_ENCODABLE_VALUES_H_
