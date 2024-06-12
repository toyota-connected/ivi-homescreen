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

#ifndef PLUGINS_COMMON_JSON_JSON_UTILS_H_
#define PLUGINS_COMMON_JSON_JSON_UTILS_H_

#include "config/common.h"

#include "rapidjson/rapidjson.h"

#include "rapidjson/document.h"

namespace plugin_common::JsonUtils {

/**
 * @brief Function to get JSON Document from a File
 * @param path pointer to file path
 * @param missing_is_error print errors if file is not found
 * @return rapidjson::Document
 * @retval Returns credential object. empty object if failed
 * @relation
 * google_sign_in
 */
rapidjson::Document GetJsonDocumentFromFile(
    std::string& path,
    bool missing_is_error = false);

/**
 * @brief Function to write JSON Document to File
 * @param path pointer to file path
 * @param doc rapidjson::Document to write to file
 * @return bool
 * @retval Returns true if successful, false otherwise
 * @relation
 * google_sign_in
 */
bool WriteJsonDocumentToFile(std::string& path,
                                    const rapidjson::Document& doc);

/**
 * @brief Function to add empty key to file
 * @param path pointer to file path
 * @param key value of key to write
 * @return bool
 * @retval Returns true if successful, false otherwise
 * @relation
 * google_sign_in
 */
bool AddEmptyKeyToFile(std::string& path, const char* key);

}  // namespace plugin_common::JsonUtils

#endif  // PLUGINS_COMMON_JSON_JSON_UTILS_H_