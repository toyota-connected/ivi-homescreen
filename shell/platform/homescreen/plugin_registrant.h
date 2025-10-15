/*
 * Copyright 2024 Toyota Connected North America
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

#ifndef PLUGIN_REGISTRANT_H_
#define PLUGIN_REGISTRANT_H_
#include <encodable_value.h>

#include "./platform_views/platform_view_listener.h"
#include "./public/flutter_homescreen_plugin.h"

#include <method_result.h>
#include <memory>

void PluginsApiRegisterPlugins(FlutterDesktopEngineRef engine);

void PluginsApiPlatformViewCreate(
    FlutterDesktopEngineRef engine,
    const std::string& flutter_asset_directory,
    const flutter::EncodableValue* arguments,
    PlatformViewAddListener addListener,
    PlatformViewRemoveListener removeListener,
    void* platform_view_context,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

#endif  // PLUGIN_REGISTRANT_H_