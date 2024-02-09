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

#include "mouse_cursor_plugin.h"

#include "messages.h"

#include "plugins/common/common.h"

namespace mouse_cursor_plugin {

// static
void MouseCursorPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarDesktop* registrar) {
  auto plugin = std::make_unique<MouseCursorPlugin>();

  MouseCursorApi::SetUp(registrar->messenger(), plugin.get());

  registrar->AddPlugin(std::move(plugin));
}

bool MouseCursorPlugin::ActivateSystemCursor(int32_t device,
                                             const std::string& kind) {
  spdlog::debug("[MouseCursor] ActivateSystemCursor: device: {}, kind: {}",
                device, kind);
  return true;
}

}  // namespace mouse_cursor_plugin