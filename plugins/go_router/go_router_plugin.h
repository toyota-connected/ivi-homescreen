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

#ifndef FLUTTER_PLUGIN_GO_ROUTER_PLUGIN_H
#define FLUTTER_PLUGIN_GO_ROUTER_PLUGIN_H

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>

#include "messages.h"

namespace go_router_plugin {

class GoRouterPlugin final : public flutter::Plugin, public GoRouterApi {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar);

  GoRouterPlugin() = default;

  ~GoRouterPlugin() override = default;

  // Disallow copy and assign.
  GoRouterPlugin(const GoRouterPlugin&) = delete;
  GoRouterPlugin& operator=(const GoRouterPlugin&) = delete;
};
}  // namespace go_router_plugin

#endif  // FLUTTER_PLUGIN_GO_ROUTER_PLUGIN_H