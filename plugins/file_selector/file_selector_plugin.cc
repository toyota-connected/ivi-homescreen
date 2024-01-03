/*
 * Copyright 2020-2023 Toyota Connected North America
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

#include "file_selector_plugin.h"

#include "messages.h"

#include <flutter/plugin_registrar.h>

#include <memory>

namespace plugin_file_selector {

// static
void FileSelectorPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrar* registrar) {
  auto plugin = std::make_unique<FileSelectorPlugin>();

  FileSelectorApi::SetUp(registrar->messenger(), plugin.get());

  registrar->AddPlugin(std::move(plugin));
}

FileSelectorPlugin::FileSelectorPlugin() = default;

FileSelectorPlugin::~FileSelectorPlugin() = default;

}  // namespace plugin_file_selector
