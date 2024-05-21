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

#ifndef FLUTTER_PLUGIN_INTEGRATION_TEST_PLUGIN_H_
#define FLUTTER_PLUGIN_INTEGRATION_TEST_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>

#include "messages.h"

namespace integration_test_plugin {

class IntegrationTestPlugin final : public flutter::Plugin,
                                    public IntegrationTestApi {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar);

  IntegrationTestPlugin() = default;

  ~IntegrationTestPlugin() override = default;

  void ArgResults(const flutter::EncodableMap& map) override;
};
}  // namespace integration_test_plugin
#endif  // FLUTTER_PLUGIN_INTEGRATION_TEST_PLUGIN_H_
