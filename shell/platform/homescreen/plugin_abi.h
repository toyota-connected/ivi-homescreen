// Copyright 2026 Toyota Connected North America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef SHELL_PLATFORM_HOMESCREEN_PLUGIN_ABI_H_
#define SHELL_PLATFORM_HOMESCREEN_PLUGIN_ABI_H_

// NOLINTNEXTLINE(modernize-deprecated-headers): C-compatible header.
#include <stdint.h>

#include "flutter/shell/platform/common/public/flutter_plugin_registrar.h"

// Stable ABI contract between the homescreen host and a plugin.
//
// Bump kHomescreenPluginAPIVersion whenever the contract below changes
// in a way that is not source- and binary-compatible with prior plugins:
//   - Signature of PluginRegisterFn changes.
//   - New required exported symbol added.
//   - Semantics of an existing exported symbol change.
//
// The host compares its compile-time kHomescreenPluginAPIVersion against
// the value returned by GetPluginAPIVersion() in each loaded plugin and
// refuses to register plugins reporting a different version.

#ifdef __cplusplus
extern "C" {
#endif

static const uint32_t kHomescreenPluginAPIVersion = 1u;

// Every plugin built as a dlopen-able MODULE must export this symbol.
// Static (OBJECT) plugins are version-checked at link time via the
// generated registry and need not export it, but the macro below is
// safe to include in either build mode.
// NOLINTNEXTLINE(modernize-redundant-void-arg): C-compatible declaration.
uint32_t GetPluginAPIVersion(void);

// Signature of a plugin's registration entry point. Each plugin exports
// its own canonically-named registrar (e.g.
// AudioplayersLinuxPluginRegisterWithRegistrar); this typedef documents
// the contract the host invokes through dlsym for dynamic plugins or
// through the generated static registry for static plugins.
typedef void (*HomescreenPluginRegisterFn)(
    FlutterDesktopPluginRegistrarRef registrar);

// NOLINTNEXTLINE(modernize-redundant-void-arg): C-compatible typedef.
typedef uint32_t (*HomescreenPluginGetVersionFn)(void);

#ifdef __cplusplus
}  // extern "C"
#endif

// Convenience: a plugin translation unit may define
// HOMESCREEN_PLUGIN_IMPL before including this header to get a free
// definition of GetPluginAPIVersion(). Define it in exactly one .cc per
// plugin.
#ifdef HOMESCREEN_PLUGIN_IMPL
#ifdef __cplusplus
extern "C" {
#endif
// NOLINTNEXTLINE(modernize-redundant-void-arg): C-compatible definition.
uint32_t GetPluginAPIVersion(void) {
  return kHomescreenPluginAPIVersion;
}
#ifdef __cplusplus
}  // extern "C"
#endif
#endif  // HOMESCREEN_PLUGIN_IMPL

#endif  // SHELL_PLATFORM_HOMESCREEN_PLUGIN_ABI_H_
