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

#ifndef SHELL_PLATFORM_HOMESCREEN_PUBLIC_FLUTTER_HOMESCREEN_PLUGIN_H
#define SHELL_PLATFORM_HOMESCREEN_PUBLIC_FLUTTER_HOMESCREEN_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#include <vector>
#include <string>

#include "flutter_export.h"
#include "flutter_messenger.h"
#include "flutter_plugin_registrar.h"

#include "./flutter_homescreen.h"
#include "../platform_views/platform_view_listener.h"

typedef struct FlutterDesktopEngineState* FlutterDesktopEngineRef;

// Function type for PluginCApiRegisterWithRegistrar functions
typedef void (*PluginApiRegisterFunc)(
    FlutterDesktopPluginRegistrarRef registrar
);

typedef void (*PluginLibraryRegisterFunc)(
    FlutterDesktopEngineRef engine
);

// Function type for PluginCApiPlatformViewCreate functions
typedef void (*PluginApiPlatformViewCreateFunc)(
    FlutterDesktopPluginRegistrarRef registrar,
    int32_t id,
    const std::string& viewType,
    int32_t direction,
    double top,
    double left,
    double width,
    double height,
    const std::vector<uint8_t>& params,
    const std::string& flutter_asset_directory,
    FlutterDesktopEngineRef engine,
    PlatformViewAddListener addListener,
    PlatformViewRemoveListener removeListener,
    void* platform_view_context
);

typedef bool (*PluginLibraryTryPlatformCreate)(
    FlutterDesktopPluginRegistrarRef registrar,
    int32_t id,
    const std::string& viewType,
    int32_t direction,
    double top,
    double left,
    double width,
    double height,
    const std::vector<uint8_t>& params,
    const std::string& flutter_asset_directory,
    FlutterDesktopEngineRef engine,
    PlatformViewAddListener addListener,
    PlatformViewRemoveListener removeListener,
    void* platform_view_context
);

typedef struct FlutterPlugin {
    const char* name;
    PluginApiRegisterFunc doRegister;

    const char* platformViewName;
    PluginApiPlatformViewCreateFunc createPlatformView;
} FlutterPlugin;

typedef struct FlutterPluginLibrary {
    const char* name;
    PluginLibraryRegisterFunc doRegister;
    PluginLibraryTryPlatformCreate tryCreatePlatformView;
} FlutterPluginLibrary;

#endif  // SHELL_PLATFORM_HOMESCREEN_PUBLIC_FLUTTER_HOMESCREEN_PLUGIN_H
