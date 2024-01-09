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

#include "desktop_window_plugin.h"

#include "messages.h"

#include "plugins/common/common.h"

namespace desktop_window_linux_plugin {

// static
void DesktopWindowLinuxPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrar* registrar) {
  auto plugin = std::make_unique<DesktopWindowLinuxPlugin>();

  DesktopWindowLinuxApi::SetUp(registrar->messenger(), plugin.get());

  registrar->AddPlugin(std::move(plugin));
}

void DesktopWindowLinuxPlugin::SetMinWindowSize(
    double width,
    double height,
    std::function<void(std::optional<FlutterError> reply)> result) {
  spdlog::debug("[desktop_window] setMinWindowSize: width: {}, height: {}",
                width, height);
  // TODO set compositor surface size to not be less than specified
  result(std::nullopt);
}

}  // namespace desktop_window_linux_plugin