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

#include "integration_test_plugin.h"

#include <memory>

#include "../common/logging.h"
#include "messages.h"

namespace integration_test_plugin {

// static
void IntegrationTestPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrar* registrar) {
  auto plugin = std::make_unique<IntegrationTestPlugin>();

  IntegrationTestApi::SetUp(registrar->messenger(), plugin.get());

  registrar->AddPlugin(std::move(plugin));
}

void IntegrationTestPlugin::ArgResults(const flutter::EncodableMap& map) {
  for (const auto& results : map) {
    auto k = std::get<std::string>(results.first);
    auto v = std::get<std::string>(results.second);
    spdlog::debug("{}={}", k, v);
  }
}

}  // namespace integration_test_plugin