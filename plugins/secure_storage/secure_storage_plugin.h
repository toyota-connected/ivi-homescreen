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

#ifndef FLUTTER_PLUGIN_SECURE_STORAGE_PLUGIN_H
#define FLUTTER_PLUGIN_SECURE_STORAGE_PLUGIN_H

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>
#include <libsecret/secret.h>

#include "keyring.h"
#include "messages.h"

namespace plugin_secure_storage {

class SecureStoragePlugin final : public flutter::Plugin,
                                  public SecureStorageApi {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar);

  SecureStoragePlugin();

  ~SecureStoragePlugin() override;

  void deleteIt(const char* key) override;

  void deleteAll() override;

  void write(const char* key, const char* value) override;

  flutter::EncodableValue read(const char* key) override;

  flutter::EncodableValue readAll() override;

  flutter::EncodableValue containsKey(const char* key) override;

  // Disallow copy and assign.
  SecureStoragePlugin(const SecureStoragePlugin&) = delete;
  SecureStoragePlugin& operator=(const SecureStoragePlugin&) = delete;

 private:
  Keyring keyring_;
};

}  // namespace plugin_secure_storage

#endif  // FLUTTER_PLUGIN_SECURE_STORAGE_PLUGIN_H
