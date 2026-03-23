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

#ifndef GENERATED_PLUGIN_REGISTRANT_H_
#define GENERATED_PLUGIN_REGISTRANT_H_
#include <encodable_value.h>

#include "plugin/platform_view_listener.h"

#include <method_result.h>
#include <memory>

typedef struct FlutterDesktopEngineState* FlutterDesktopEngineRef;

void PluginsApiRegisterPlugins(FlutterDesktopEngineRef engine);

#endif  // GENERATED_PLUGIN_REGISTRANT_H_
