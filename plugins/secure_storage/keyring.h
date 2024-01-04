/*
 * Copyright 2020-2024 Toyota Connected North America
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

#include <memory>

#include <libsecret/secret.h>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>

#include "hash_table.h"
#include "logging/logging.h"

namespace plugin_secure_storage {

class Keyring {
  HashTable attributes_;
  std::string label_;
  SecretSchema schema_{};

 public:
  explicit Keyring(const char* label = "default") : label_(label) {
    schema_ = {label_.c_str(),
               SECRET_SCHEMA_NONE,
               {
                   {"account", SECRET_SCHEMA_ATTRIBUTE_STRING},
               }};
  }

  bool addItem(const char* key, const char* value) {
    rapidjson::Document root = readFromKeyring();
    if (root.IsObject() && root.HasMember(key) && root[key].IsString()) {
      root.RemoveMember(key);
    }
    rapidjson::Value k(key, root.GetAllocator());
    rapidjson::Value v(value, root.GetAllocator());
    root.AddMember(k, v, root.GetAllocator());
    return this->storeToKeyring(root);
  }

  std::string getItem(const char* key) {
    rapidjson::Document root = readFromKeyring();
    if (root.IsObject() && root.HasMember(key) && root[key].IsString()) {
      return root[key].GetString();
    }
    return "";
  }

  void deleteItem(const char* key) {
    rapidjson::Document root = readFromKeyring();
    if (root.HasMember(key)) {
      root.RemoveMember(key);
    }
    this->storeToKeyring(root);
  }

  bool deleteKeyring() {
    rapidjson::Document d;
    d.SetObject();
    return this->storeToKeyring(d);
  }

  bool storeToKeyring(rapidjson::Document& d) {
    std::unique_ptr<GError> err = nullptr;
    GError* errPtr = err.get();

    rapidjson::StringBuffer buffer;
    buffer.Clear();
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);

    auto res = (bool)secret_password_storev_sync(
        &schema_, attributes_.getGHashTable(), nullptr, label_.c_str(),
        buffer.GetString(), nullptr, &errPtr);

    auto result = static_cast<bool>(res);

    if (err) {
      throw std::runtime_error(err->message);
    }

    return result;
  }

  rapidjson::Document readFromKeyring() {
    rapidjson::Document d;
    std::unique_ptr<GError> err = nullptr;
    GError* errPtr = err.get();

    const gchar* result = secret_password_lookupv_sync(
        &schema_, attributes_.getGHashTable(), nullptr, &errPtr);

    if (err) {
      throw std::runtime_error(err->message);
    }

    if (result != nullptr && strcmp(result, "") != 0 &&
        !d.Parse(result).HasParseError()) {
      if (strcmp(result, "null") == 0) {
        const char* json = "{}";
        d.Parse(json);
      }
      return d;
    }

    this->storeToKeyring(d);
    return d;
  }
};

}  // namespace plugin_secure_storage
