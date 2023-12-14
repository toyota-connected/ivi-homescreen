/*
 * Copyright 2023, the Chromium project authors.  Please see the AUTHORS file
 * for details. All rights reserved. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 * Copyright 2023, Toyota Connected North America
 */

#ifndef FLUTTER_PLUGIN_FILAMENT_VIEW_PLUGIN_C_API_H_
#define FLUTTER_PLUGIN_FILAMENT_VIEW_PLUGIN_C_API_H_

#include <flutter_plugin_registrar.h>
#include "flutter_homescreen.h"

#include <string>
#include <vector>

#ifdef FLUTTER_PLUGIN_IMPL
#define FLUTTER_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define FLUTTER_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#if defined(__cplusplus)
extern "C" {
#endif

FLUTTER_PLUGIN_EXPORT void FilamentViewPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrar* registrar,
    int32_t id,
    std::string viewType,
    int32_t direction,
    double width,
    double height,
    const std::vector<uint8_t>& params,
    std::string assetDirectory,
    FlutterDesktopEngineRef engine);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // FLUTTER_PLUGIN_FILAMENT_VIEW_PLUGIN_C_API_H_
