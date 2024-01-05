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

#include "include/webview_flutter/webview_flutter_plugin_c_api.h"

#include "flutter/plugin_registrar.h"

#include "webview_flutter_plugin.h"

void WebviewFlutterPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrar* registrar,
    int32_t id,
    std::string viewType,
    int32_t direction,
    double top,
    double left,
    double width,
    double height,
    const std::vector<uint8_t>& params,
    std::string assetDirectory,
    FlutterDesktopEngineRef engine,
    PlatformViewAddListener add_listener,
    PlatformViewRemoveListener remove_listener,
    void* platform_views_context) {
  plugin_webview_flutter::WebviewFlutterPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrar>(registrar),
      id, std::move(viewType), direction, top, left, width, height, params,
      std::move(assetDirectory), engine, add_listener, remove_listener,
      platform_views_context);
}
