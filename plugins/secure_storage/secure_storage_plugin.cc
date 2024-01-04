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

#include "secure_storage_plugin.h"

#include "messages.h"

#include <flutter/plugin_registrar.h>

namespace plugin_secure_storage {

// static
void SecureStoragePlugin::RegisterWithRegistrar(
    flutter::PluginRegistrar* registrar) {
  auto plugin = std::make_unique<SecureStoragePlugin>();

  SecureStorageApi::SetUp(registrar->messenger(), plugin.get());

  registrar->AddPlugin(std::move(plugin));
}

SecureStoragePlugin::SecureStoragePlugin() = default;

SecureStoragePlugin::~SecureStoragePlugin() = default;

void SecureStoragePlugin::deleteIt(const char* key) {
  keyring_.deleteItem(key);
}

void SecureStoragePlugin::deleteAll() {
  keyring_.deleteKeyring();
}

void SecureStoragePlugin::write(const char* key, const char* value) {
  keyring_.addItem(key, value);
}

flutter::EncodableValue SecureStoragePlugin::read(const char* key) {
  auto str = keyring_.getItem(key);
  if (str.empty()) {
    return flutter::EncodableValue();
  }
  return flutter::EncodableValue(str);
}

flutter::EncodableValue SecureStoragePlugin::readAll() {
  auto result = flutter::EncodableMap{};
  auto document = keyring_.readFromKeyring();
  if (document.IsObject()) {
    for (rapidjson::Value::ConstMemberIterator itr = document.MemberBegin();
         itr != document.MemberEnd(); ++itr) {
      result.emplace(flutter::EncodableValue(itr->name.GetString()),
                     flutter::EncodableValue(itr->value.GetString()));
    }
  }
  return flutter::EncodableValue(result);
}

flutter::EncodableValue SecureStoragePlugin::containsKey(const char* key) {
  auto document = keyring_.readFromKeyring();
  return flutter::EncodableValue(document.HasMember(key));
}

}  // namespace plugin_secure_storage
