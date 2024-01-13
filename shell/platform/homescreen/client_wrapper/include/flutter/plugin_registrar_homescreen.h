/*
 * Copyright 2023 Toyota Connected North America
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

#ifndef SHELL_PLATFORM_HOMESCREEN_CLIENT_WRAPPER_INCLUDE_FLUTTER_PLUGIN_REGISTRAR_HOMESCREEN_H_
#define SHELL_PLATFORM_HOMESCREEN_CLIENT_WRAPPER_INCLUDE_FLUTTER_PLUGIN_REGISTRAR_HOMESCREEN_H_

#include <flutter_homescreen.h>

#include "plugin_registrar.h"

class FlutterView;

namespace flutter {

class PluginRegistrarDesktop final : public PluginRegistrar {
 public:
  explicit PluginRegistrarDesktop(
      FlutterDesktopPluginRegistrarRef core_registrar)
      : PluginRegistrar(core_registrar) {}

  ~PluginRegistrarDesktop() override { ClearPlugins(); }

  FlutterView* GetView() const { return nullptr; }

  // Prevent copying.
  PluginRegistrarDesktop(PluginRegistrarDesktop const&) = delete;
  PluginRegistrarDesktop& operator=(PluginRegistrarDesktop const&) = delete;
};

}  // namespace flutter

#endif  // SHELL_PLATFORM_HOMESCREEN_CLIENT_WRAPPER_INCLUDE_FLUTTER_PLUGIN_REGISTRAR_HOMESCREEN_H_
