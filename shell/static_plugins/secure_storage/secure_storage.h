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
  static void OnPlatformMessage(const FlutterPlatformMessage* message,
                                void* userdata);

  static void deleteIt(const char* key);
  static void deleteAll();
  static void write(const char* key, const char* value);
  static flutter::EncodableValue read(const char* key);
  static flutter::EncodableValue readAll();
  static flutter::EncodableValue containsKey(const char* key);

  SecureStorage(SecureStorage& other) = delete;
  void operator=(const SecureStorage&) = delete;
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
