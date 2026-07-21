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

#include <functional>
#include <memory>

#include "config/common.h"
#include "configuration/configuration.h"
#include "profiling/motion_to_photon.h"

#include <flutter_texture_registrar.h>
#include <shell/platform/embedder/embedder.h>

#if BUILD_COMPOSITOR
#include "view/compositor_surface_interface.h"
#endif

#if BUILD_HUD
#include "backend/hud/ihud.h"
#endif

class Engine;
class TaskRunner;
class IDisplay;

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
  void* gbm_device;     // struct gbm_device* on a gbm/DRM backend (NULL on a
                        // Wayland EGL backend). Lets a platform view allocate a
                        // scanout-capable buffer it can export as a dma-buf.
};

// Carries the Vulkan handle set a plugin needs to render into a VkImage using
// the SAME device Flutter renders with (so a platform view's output is a
// first-class Vulkan resource the compositor can later import — no second
// device, no cross-API copy). All handles are |void*| so the public embedder
// header avoids a hard Vulkan include; callers cast them back to the matching
// Vk types. Only Vulkan-based backends override GetVulkanContext.
struct BackendVulkanContext {
  void* instance;         // VkInstance
  void* physical_device;  // VkPhysicalDevice
  void* device;           // VkDevice
  void* queue;            // VkQueue (externally synchronized — see note below)
  uint32_t queue_family_index;
  void* get_instance_proc_addr;  // PFN_vkGetInstanceProcAddr (shared loader)
  // The device extensions the backend enabled, so a plugin can gate optional
  // paths (e.g. sync_file semaphore export for explicit-sync acquire) on what
  // the shared device actually supports. Backed by backend-owned storage that
  // outlives the context; null/0 when the backend does not report them.
  const char* const* device_extensions;
  size_t device_extension_count;
};

// The unified display-target interface: a Backend owns surface lifecycle, the
// Flutter renderer/compositor config, and vsync for one presentation target
// (Wayland compositor, DRM-KMS, software dumb buffer). One factory
// (Backend::Create) builds the right concrete backend per FlutterView, and each
// instance is isolated so heterogeneous backends can run concurrently. The
// concrete implementations are the *Backend classes (WaylandEglBackend,
// DrmBackend, SoftwareBackend, ...). Distinct from the Wayland
// compositor-protocol shells in wayland/shell/ (the --shell option).
class Backend {
 public:
  Backend() = default;
  virtual ~Backend() = default;

  Backend(const Backend&) = delete;
  const Backend& operator=(const Backend&) = delete;

#if BUILD_HUD
  // Debug-HUD appearance/layout + whether config enabled it, translated from
  // the view's [hud] table (see register_backends). The concrete backend
  // applies hud_config_ to its HUD and treats hud_config_enable_ as an enable
  // source in addition to the IVI_HUD env var. Set once, before the first
  // present.
  void SetHudConfig(const ihs::hud::HudConfig& config, bool enable) {
    hud_config_ = config;
    hud_config_enable_ = enable;
  }
#endif

  /**
   * @brief The single factory for a per-view Backend. Selects the concrete
   * implementation (Wayland-EGL/-Vulkan, DRM-KMS-EGL/-Vulkan, software)
   * from the compiled-in backends + @p config, constructs an isolated
   * instance, and performs any backend-specific post-creation wiring (e.g. DRM
   * cursor/viewport). Replaces the #if/#elif chain that used to live inline in
   * FlutterView. Calls exit() on a hard backend-init failure. Defined in
   * backend/shell_factory.cc.
   */
  [[nodiscard]] static std::shared_ptr<Backend> Create(
      const Configuration::Config& config,
      const std::shared_ptr<IDisplay>& display);

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
   * @brief Fill |out| with the backend's Vulkan handles so a plugin can render
   *        a platform view into a VkImage on the SAME device Flutter uses. Only
   *        Vulkan-based backends override this; the default returns false (the
   *        plugin then falls back to its GL path or declines).
   *
   * The queue is the backend's single graphics/present queue and is externally
   * synchronized: a plugin submitting on it must serialize with the embedder
   * (the wayland_vulkan backend routes engine submissions through a shared
   * mutex via QueueInterposer — a plugin should submit from the raster thread
   * inside OnPresent and wait its own fence, not hold work across frames).
   */
  virtual bool GetVulkanContext(BackendVulkanContext* /* out */) const {
    return false;
  }

  // Run @fn at a point where no compositor frame is recording or in flight —
  // the top of a later present, after prior frame fences. A platform-view host
  // uses this to free an imported image on dispose without racing the raster
  // thread that may be mid-record binding it. Default: run now (no compositor).
  virtual void ScheduleDeferredDestroy(std::function<void()> fn) {
    if (fn) {
      fn();
    }
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

  /**
   * @brief Release the backend's GPU / windowing-system resources (render
   * surface, native window, GL contexts, display connection).
   *
   * Called from @c FlutterView::~FlutterView @e after the engine has been
   * stopped and all its threads joined, but while the window/display members
   * are still alive. Tearing these down before the rasterizer thread is
   * joined races that thread on the GL context (driver heap corruption);
   * tearing them down after the window member is destroyed is a use-after-free
   * on the native surface. This hook is the one safe point between the two.
   * Must be idempotent. Default is a no-op.
   */
  virtual void ReleaseRenderSurfaces() {}

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

  /**
   * @brief The motion-to-photon profiler for this backend, or nullptr when it
   * is not profiling.
   *
   * The DRM/KMS backends own an instance and opt in via InitMotionToPhoton()
   * (enabled by IVI_M2P_PROFILE); they feed RecordPresent from the page-flip
   * path and the input seat feeds RecordInput, both marshaled onto the platform
   * task runner. The Wayland backends never opt in — their profiler lives on
   * Display — so this returns nullptr for them.
   */
  [[nodiscard]] profiling::MotionToPhoton* GetMotionToPhoton() {
    return m2p_enabled_ ? &m2p_ : nullptr;
  }

 protected:
  /// Enable the motion-to-photon profiler for this backend if IVI_M2P_PROFILE
  /// is set. Called from the DRM/KMS backend constructors.
  void InitMotionToPhoton() {
    m2p_enabled_ = profiling::MotionToPhoton::Enabled();
  }

  profiling::MotionToPhoton m2p_;
  bool m2p_enabled_ = false;

#if BUILD_HUD
  ihs::hud::HudConfig hud_config_{};
  bool hud_config_enable_{false};
#endif
};
