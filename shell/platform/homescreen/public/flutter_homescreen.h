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

#ifndef SHELL_PLATFORM_HOMESCREEN_PUBLIC_FLUTTER_HOMESCREEN_H
#define SHELL_PLATFORM_HOMESCREEN_PUBLIC_FLUTTER_HOMESCREEN_H

#include <stddef.h>
#include <stdint.h>

#include "flutter_export.h"
#include "flutter_messenger.h"
#include "flutter_plugin_registrar.h"

#if defined(__cplusplus)
extern "C" {
#endif

// Opaque reference to a Flutter window controller.
typedef struct FlutterDesktopViewControllerState*
    FlutterDesktopWindowControllerRef;

// Opaque reference to a Flutter window.
typedef struct FlutterDesktopView* FlutterDesktopWindowRef;

// Opaque reference to a Flutter engine instance.
typedef struct FlutterDesktopEngineState* FlutterDesktopEngineRef;

// Returns the window handle for the window associated with
// FlutterDesktopWindowControllerRef.
//
// Its lifetime is the same as the |controller|'s.
FLUTTER_EXPORT FlutterDesktopWindowRef
FlutterDesktopGetWindow(FlutterDesktopWindowControllerRef controller);

// Returns the handle for the engine running in
// FlutterDesktopWindowControllerRef.
//
// Its lifetime is the same as the |controller|'s.
FLUTTER_EXPORT FlutterDesktopEngineRef
FlutterDesktopGetEngine(FlutterDesktopWindowControllerRef controller);

// Returns the plugin registrar handle for the plugin with the given name.
//
// The name must be unique across the application.
FLUTTER_EXPORT FlutterDesktopPluginRegistrarRef
FlutterDesktopGetPluginRegistrar(FlutterDesktopEngineRef engine,
                                 const char* plugin_name);

// EGL handles that let a plugin create its own EGL context sharing GL
// objects with the embedder's raster context. Fields are |void*| so the
// header does not require |<EGL/egl.h>|; callers cast to the matching EGL
// types (|EGLDisplay|, |EGLConfig|, |EGLContext|).
typedef struct {
  void* display;
  void* config;
  void* share_context;
} FlutterDesktopEglContext;

// Populates |out| with EGL handles the plugin can use to create an EGL
// context on its own thread. The plugin must pass |share_context| as the
// |share_context| argument of |eglCreateContext| so that GL objects (e.g.
// textures) it creates are visible on Flutter's raster thread for use with
// |kFlutterDesktopGpuSurfaceTypeGlTexture2D| external textures.
//
// Returns false if the embedder's current backend is not EGL-based
// (e.g. Vulkan) or the handles are not yet initialized.
FLUTTER_EXPORT bool FlutterDesktopPluginRegistrarGetEglContext(
    FlutterDesktopPluginRegistrarRef registrar,
    FlutterDesktopEglContext* out);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // SHELL_PLATFORM_HOMESCREEN_PUBLIC_FLUTTER_HOMESCREEN_H
