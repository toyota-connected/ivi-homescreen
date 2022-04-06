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

#include <json/json.h>
#include <libsecret/secret.h>
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

  bool addItem(const char* key, const char* value) {
    Json::Value root = readFromKeyring();
    root[key] = value;
    return this->storeToKeyring(root);
  }

  std::string getItem(const char* key) {
    Json::Value root = readFromKeyring();
    Json::Value resultJson = root[key];
    if (resultJson.isString()) {
      return resultJson.asString();
    }
    return "";
  }

  void deleteItem(const char* key) {
    Json::Value root = readFromKeyring();
    root.removeMember(key);
    this->storeToKeyring(root);
  }

  bool deleteKeyring() { return this->storeToKeyring(Json::Value()); }

  bool storeToKeyring(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    const std::string output = Json::writeString(builder, value);
    std::unique_ptr<GError> err = nullptr;
    GError* errPtr = err.get();

    builder["indentation"] = "";

    bool result = secret_password_storev_sync(
        &m_schema, m_attributes.getGHashTable(), nullptr, m_label.c_str(),
        output.c_str(), nullptr, &errPtr);

    if (err) {
      throw std::runtime_error(err->message);
    }

    return result;
  }

  Json::Value readFromKeyring() {
    Json::Value root;
    Json::CharReaderBuilder charBuilder;
    std::unique_ptr<Json::CharReader> reader(charBuilder.newCharReader());
    std::unique_ptr<GError> err = nullptr;
    GError* errPtr = err.get();

    const gchar* result = secret_password_lookupv_sync(
        &m_schema, m_attributes.getGHashTable(), nullptr, &errPtr);

    if (err) {
      throw std::runtime_error(err->message);
    }

    if (result != nullptr && strcmp(result, "") != 0 &&
        reader->parse(result, result + strlen(result), &root, nullptr)) {
      return root;
    }

    this->storeToKeyring(root);
    return root;
  }
};

}  // namespace secret
