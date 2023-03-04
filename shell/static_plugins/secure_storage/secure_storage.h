/*
 * Copyright 2020 Toyota Connected North America
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

#include <flutter_embedder.h>

#include <mutex>
#include <utility>
#include "flutter/encodable_value.h"
#include "keyring.h"

class SecureStorage {
 public:
  static constexpr char kChannelName[] =
      "plugins.it_nomads.com/flutter_secure_storage";

  /**
  * @brief Callback function for platform messages about secure storage
  * @param[in] message Recieve message
  * @param[in] userdata Pointer to User data
  * @return void
  * @relation
  * flutter
  */
  static void OnPlatformMessage(const FlutterPlatformMessage* message,
                                void* userdata);

  /**
  * @brief Delete a key and value from secure storage
  * @param[in] key A key to delete
  * @return void
  * @relation
  * flutter
  */
  static void deleteIt(const char* key);

  /**
  * @brief Empty secure storage
  * @return void
  * @relation
  * flutter
  */
  static void deleteAll();

  /**
  * @brief Write new a key and value to secure storage
  * @param[in] key A key to write
  * @param[in] value The value to associate with the key
  * @return void
  * @relation
  * flutter
  */
  static void write(const char* key, const char* value);

  /**
  * @brief Read the string associate with a key from secure storage
  * @param[in] key A key to read string
  * @return flutter::EncodableValue
  * @retval The associated string, or "" if the key is not found
  * @relation
  * flutter
  */
  static flutter::EncodableValue read(const char* key);

  /**
  * @brief Read all keys and values from secure storage
  * @return flutter::EncodableValue
  * @retval All keys and values
  * @relation
  * flutter
  */
  static flutter::EncodableValue readAll();

  /**
  * @brief Check if key is in secure storage
  * @param[in] key A key to check
  * @return flutter::EncodableValue
  * @retval true If key is in secure storage
  * @retval false Otherwise
  * @relation
  * flutter
  */
  static flutter::EncodableValue containsKey(const char* key);

  SecureStorage(SecureStorage& other) = delete;
  void operator=(const SecureStorage&) = delete;

  /**
  * @brief Get instance of SecureStorage class
  * @param[in] value Default argument
  * @return SecureStorage*
  * @retval Instance of the SecureStorage class
  * @relation
  * flutter
  */
  static SecureStorage* GetInstance(const std::string& value);

 private:
  static constexpr char kKey[] = "key";
  static constexpr char kValue[] = "value";
  static constexpr char kWrite[] = "write";
  static constexpr char kRead[] = "read";
  static constexpr char kReadAll[] = "readAll";
  static constexpr char kDelete[] = "delete";
  static constexpr char kDeleteAll[] = "deleteAll";
  static constexpr char kContainsKey[] = "containsKey";

  secret::Keyring keyring_;

 protected:
  explicit SecureStorage(std::string value) : value_(std::move(value)) {}
  ~SecureStorage() = default;
  std::string value_;
};
