/*
 * Copyright 2026 Toyota Connected North America
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

#pragma once

#include "./platform_view_listener.h"

#include <flutter/encodable_value.h>
#include <flutter/method_result.h>

#include <memory>

typedef struct FlutterDesktopEngineState* FlutterDesktopEngineRef;

void FlutterPluginRegister(FlutterDesktopEngineRef engine);
typedef void (*FlutterPluginRegisterCallback)(FlutterDesktopEngineRef engine);

void FlutterPluginPlatformViewCreate(
    FlutterDesktopEngineRef engine,
    const std::string& flutter_asset_directory,
    const flutter::EncodableValue* arguments,
    PlatformViewAddListener addListener,
    PlatformViewRemoveListener removeListener,
    void* platform_view_context,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
typedef void (*FlutterPluginPlatformViewCreateRef)(
    FlutterDesktopEngineRef engine,
    const std::string& flutter_asset_directory,
    const flutter::EncodableValue* arguments,
    PlatformViewAddListener addListener,
    PlatformViewRemoveListener removeListener,
    void* platform_view_context,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);