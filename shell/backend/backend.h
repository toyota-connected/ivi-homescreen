/*
 * Copyright 2022 Toyota Connected North America
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

#include <memory>

#include "config/common.h"

#include <flutter_texture_registrar.h>
#include <shell/platform/embedder/embedder.h>

#if BUILD_COMPOSITOR
#include "view/compositor_surface_interface.h"
#endif

class Engine;
class TaskRunner;

// Carries an EGL context handle set that lets a plugin create its own EGL
// context sharing GL objects with the embedder's raster context. All handles
// are |void*| so the public embedder header can avoid a hard EGL include;
// callers cast them back to the matching EGL types.
struct BackendEglContext {
  void* display;        // EGLDisplay
  void* config;         // EGLConfig
  void* share_context;  // EGLContext suitable as share_context for a
                        // plugin-owned context that uploads GL objects
                        // visible on the raster thread.
};

class Backend {
 public:
  enum Type {
    Headless,
    WaylandEgl,
    WaylandVulkan,
    WaylandLeasedDrm,
    DrmKms,
  };

  Backend() = default;
  virtual ~Backend() = default;

  Backend(const Backend&) = delete;
  const Backend& operator=(const Backend&) = delete;

  /**
   * @brief Execute the callback function for window resizing
   * @param[in] index Set Application ID
   * @param[in] flutter_engine Pointer to Flutter engine
   * @param[in] width Set window width
   * @param[in] height Set window height
   * @return void
   * @relation
   * flutter, wayland
   */
  virtual void Resize(size_t index,
                      Engine* flutter_engine,
                      int32_t width,
                      int32_t height) = 0;

  /**
   * @brief Execute the callback function for surface creating
   * @param[in] index Set Application ID
   * @param[in] surface Pointer to surface
   * @param[in] width Set surface width
   * @param[in] height Set surface height
   * @return void
   * @relation
   * wayland, (flutter)
   */
  virtual void CreateSurface(size_t index,
                             struct wl_surface* surface,
                             int32_t width,
                             int32_t height) = 0;

  virtual bool TextureMakeCurrent() = 0;

  virtual bool TextureClearCurrent() = 0;

  /**
   * @brief Fill |out| with EGL handles usable as a |share_context| for a
   *        plugin-created EGL context. Only EGL-based backends override this;
   *        the default returns false.
   */
  virtual bool GetEglContext(BackendEglContext* /* out */) const {
    return false;
  }

  /**
   * @brief Get an empty FlutterRendererConfig
   * @return FlutterRendererConfig
   * @retval Pointer to FlutterRendererConfig
   * @relation
   * internal
   */
  virtual FlutterRendererConfig GetRenderConfig() = 0;

  /**
   * @brief Get an empty FlutterCompositor
   * @return FlutterCompositor
   * @retval Pointer to FlutterCompositor
   * @relation
   * internal
   */
  virtual FlutterCompositor GetCompositorConfig() = 0;

  /**
   * @brief Per-backend FlutterVsyncCallback.
   *
   * Returning nullptr (the default) leaves @c FlutterProjectArgs.vsync_callback
   * unset and Flutter falls back to its internal wall-clock scheduler.
   * Backends that can deliver vblank-locked OnVsync events override this and
   * return a function pointer; the engine only wires it into the embedder
   * args when non-null. The @c user_data the callback receives is the
   * @c FlutterDesktopEngineState* passed to @c FlutterEngineRun.
   *
   * Implementations MUST marshal @c FlutterEngineOnVsync calls onto the
   * platform task runner — Flutter enforces that constraint and returns
   * @c kInternalInconsistency otherwise.
   */
  virtual VsyncCallback GetVsyncCallback() const { return nullptr; }

  /**
   * @brief Hand the running FlutterEngine handle to the backend.
   *
   * Backends that wire @c FlutterEngineOnVsync (or otherwise need the
   * engine handle from outside an engine-supplied callback) override this
   * to stash the handle atomically. Default is a no-op; backends without
   * vsync_callback or session lifecycle hooks don't need it. Called from
   * FlutterView once @c FlutterEngineRun returns successfully.
   */
  virtual void SetEngineHandle(FLUTTER_API_SYMBOL(FlutterEngine) /*engine*/) {}

  /**
   * @brief Hand the platform task runner to the backend.
   *
   * Used by backends that must marshal @c FlutterEngineOnVsync (or other
   * engine APIs) onto the FlutterEngineRun thread. Default is a no-op.
   * Called from FlutterView from the same post-Engine::Run hook that
   * installs the engine handle. Backends that drive an event-source fd
   * (e.g. DRM page-flip, Wayland display) may use this hook to start an
   * asio @c async_wait on the runner's io_context.
   */
  virtual void SetPlatformTaskRunner(TaskRunner* /*runner*/) {}

  /**
   * @brief Tear down any vsync/event-loop monitor the backend started.
   *
   * Called from @c FlutterView::~FlutterView before the engine destructs
   * so the backend has a chance to cancel outstanding async work that
   * could otherwise outlive the engine handle. Default is a no-op.
   */
  virtual void StopVsyncMonitor() {}

#if BUILD_COMPOSITOR
  /**
   * @brief Register a platform-view compositor surface.
   *
   * Default is a no-op; only compositor-capable backends override. Must be
   * paired with @c UnregisterCompositorSurface.
   */
  virtual void RegisterCompositorSurface(
      FlutterPlatformViewIdentifier /*id*/,
      std::shared_ptr<ICompositorSurface> /*surface*/) {}

  virtual void UnregisterCompositorSurface(
      FlutterPlatformViewIdentifier /*id*/) {}

  /**
   * @brief Notify a registered surface that its Dart-side widget resized.
   *
   * Looks up the identifier; no-op if unregistered.
   */
  virtual void ResizeCompositorSurface(FlutterPlatformViewIdentifier /*id*/,
                                       int32_t /*width*/,
                                       int32_t /*height*/) {}
#endif
};
