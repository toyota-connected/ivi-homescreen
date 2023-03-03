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

#include <flutter/fml/logging.h>
#include <libsecret/secret.h>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <memory>
#include "g_hash_table.h"

namespace secret {

class Keyring {
  FHashTable m_attributes;
  std::string m_label;
  SecretSchema m_schema{};

 public:
  explicit Keyring(const char* _label = "default") : m_label(_label) {
    m_schema = {m_label.c_str(),
                SECRET_SCHEMA_NONE,
                {
                    {"account", SECRET_SCHEMA_ATTRIBUTE_STRING},
                }};
  }

  /**
  * @brief Add new a key and value to the keyring
  * @param[in] key A key to add
  * @param[in] value The value to associate with the key
  * @return bool
  * @retval true Normal end
  * @retval false Abnormal end
  * @relation
  * flutter
  */
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

  /**
  * @brief Get the string associate with a key from keyring
  * @param[in] key A key to get string
  * @return std::string
  * @retval The associated string, or "" if the key is not found
  * @relation
  * flutter
  */
  std::string getItem(const char* key) {
    rapidjson::Document root = readFromKeyring();
    if (root.IsObject() && root.HasMember(key) && root[key].IsString()) {
      return root[key].GetString();
    }
    return "";
  }

  /**
  * @brief Delete key and value from keyring
  * @param[in] key A key to delete
  * @return void
  * @relation
  * flutter
  */
  void deleteItem(const char* key) {
    rapidjson::Document root = readFromKeyring();
    if (root.HasMember(key)) {
      root.RemoveMember(key);
    }
    this->storeToKeyring(root);
  }

  /**
  * @brief Empty keyring
  * @return bool
  * @retval true Normal end
  * @retval false Abnormal end
  * @relation
  * flutter
  */
  bool deleteKeyring() {
    rapidjson::Document d;
    d.SetObject();
    return this->storeToKeyring(d);
  }

  /**
  * @brief Store a password in the secret service
  * @param[in] d A document for parsing JSON text as DOM
  * @return bool
  * @retval true Normal end
  * @retval false Abnormal end
  * @relation
  * flutter
  */
  bool storeToKeyring(rapidjson::Document& d) {
    std::unique_ptr<GError> err = nullptr;
    GError* errPtr = err.get();

    rapidjson::StringBuffer buffer;
    buffer.Clear();
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);

    bool result = secret_password_storev_sync(
        &m_schema, m_attributes.getGHashTable(), nullptr, m_label.c_str(),
        buffer.GetString(), nullptr, &errPtr);

    if (err) {
      throw std::runtime_error(err->message);
    }

    return result;
  }

  /**
  * @brief Lookup a password in the secret service
  * @return rapidjson::Document
  * @retval A new password string
  * @relation
  * flutter
  */
  rapidjson::Document readFromKeyring() {
    rapidjson::Document d;
    std::unique_ptr<GError> err = nullptr;
    GError* errPtr = err.get();

    const gchar* result = secret_password_lookupv_sync(
        &m_schema, m_attributes.getGHashTable(), nullptr, &errPtr);

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

}  // namespace secret
