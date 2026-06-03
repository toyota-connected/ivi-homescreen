// @copyright Copyright (c) 2022 Woven Alpha, Inc.
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

#pragma once

#include "config/common.h"

#include <memory>

#include "configuration/configuration.h"
#include "display/idisplay.h"
#include "flutter/fml/macros.h"
#include "flutter_desktop_view_controller_state.h"
#include "shell/accessibility/accessibility_tree.h"
#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN || \
    BUILD_BACKEND_HEADLESS_EGL
#include "wayland/window.h"
#endif

#include <flutter_homescreen.h>

#ifdef ENABLE_PLUGIN_COMP_SURF
#include "compositor_surface.h"
#endif
#ifdef ENABLE_PLUGIN_COMP_REGION
#include "plugins/comp_region/comp_region.h"
#endif

#if BUILD_COMPOSITOR
#include "view/compositor_surface_interface.h"
#endif

class IDisplay;
// WaylandWindow / Display are forward-declared unconditionally so
// shared_ptr<WaylandWindow> members and the GetWindow() accessor
// compile across all backends. The class definitions are only
// available when a Wayland backend is selected (wayland/window.h
// is conditionally included above); make_shared<WaylandWindow> +
// any method call require the complete type, so those sites are
// guarded with #if BUILD_BACKEND_WAYLAND_EGL || …_VULKAN. The
// non-Wayland binaries hold a default-null shared_ptr; callers
// (engine.cc, mouse_cursor_handler.cc) already null-check.
class Display;
class WaylandWindow;
class Engine;
class Backend;
class PlatformHandler;
class PlatformChannel;
#if BUILD_BACKEND_HEADLESS_EGL
class HeadlessBackend;
#elif BUILD_BACKEND_DRM_KMS_EGL
class DrmBackend;
#elif BUILD_BACKEND_DRM_KMS_VULKAN
class VulkanDrmBackend;
#elif BUILD_BACKEND_SOFTWARE
class SoftwareBackend;
#elif BUILD_BACKEND_WAYLAND_EGL
class WaylandEglBackend;
#elif BUILD_BACKEND_WAYLAND_VULKAN
class WaylandVulkanBackend;
#else
#error \
    "no Flutter backend selected: define one of BUILD_BACKEND_HEADLESS_EGL, " \
    "BUILD_BACKEND_DRM_KMS_EGL, BUILD_BACKEND_DRM_KMS_VULKAN, " \
    "BUILD_BACKEND_SOFTWARE, BUILD_BACKEND_WAYLAND_EGL, " \
    "BUILD_BACKEND_WAYLAND_VULKAN"
#endif
#ifdef ENABLE_PLUGIN_COMP_SURF
class CompositorSurface;
#endif

class FlutterView {
 public:
  FlutterView(Configuration::Config config,
              size_t index,
              const std::shared_ptr<IDisplay>& display);
  ~FlutterView();

  /**
   * @brief Run Tasks
   * @return void
   * @relation
   * wayland, flutter
   */
  void RunTasks();

  /**
   * @brief Whether this view has work that must be pumped at frame cadence.
   *
   * True when compositor-surface plugins are active (they drive their own
   * rendering via RunTask each tick). Used by App::Loop to decide whether it
   * may block idle or must keep ticking at the refresh rate.
   * @return bool
   * @relation
   * flutter
   */
  [[nodiscard]] bool NeedsPeriodicPump() const;

  /**
   * @brief Initialize
   * @return void
   * @relation
   * wayland, flutter
   */
  void Initialize();

  /**
   * @brief Get the WaylandWindow associated with this view.
   * @return The shared_ptr; default-constructed null on the DRM and
   *         software backends (no Wayland surface). Callers must
   *         null-check before dereferencing.
   */
  std::shared_ptr<WaylandWindow> GetWindow() { return m_wayland_window; }

  /**
   * @brief Get Backend
   * @return Backend*
   * @retval Backend pointer
   * @relation
   * wayland, flutter
   */
  [[nodiscard]] Backend* GetBackend() const {
    return reinterpret_cast<Backend*>(m_backend.get());
  }

  /**
   * @brief Get an index of flutter views
   * @return uint64_t
   * @retval index
   * @relation
   * internal
   */
  [[nodiscard]] uint64_t GetIndex() const { return m_index; }

  /**
   * @brief Get pointer to Display object
   * @return Display*
   * @relation
   * internal
   */
#if !BUILD_BACKEND_DRM_KMS_EGL && !BUILD_BACKEND_DRM_KMS_VULKAN
  [[nodiscard]] Display* GetDisplay() const;
#endif

#ifdef ENABLE_PLUGIN_COMP_SURF
  /**
   * @brief Create a surface ofr a compositor surface plugin
   * @param[in] h_module Handle of a module
   * @param[in] assets_path Path of assets
   * @param[in] cache_path Path of cache
   * @param[in] misc_path Path of misc
   * @param[in] type Type of a surface
   * @param[in] z_order Z order of a surface
   * @param[in] sync Sync of a surface
   * @param[in] width Width of a surface
   * @param[in] height Height of a surface
   * @param[in] x X of a surface
   * @param[in] y Y of a surface
   * @return size_t
   * @retval a memory size of a surface
   * @relation
   * plugin, wayland
   */
  size_t CreateSurface(void* h_module,
                       const std::string& assets_path,
                       const std::string& cache_path,
                       const std::string& misc_path,
                       CompositorSurface::PARAM_SURFACE_T type,
                       CompositorSurface::PARAM_Z_ORDER_T z_order,
                       CompositorSurface::PARAM_SYNC_T sync,
                       int width,
                       int height,
                       int32_t x,
                       int32_t y);

  /**
   * @brief Dispose a surface of a compositor surface plugin
   * @param[in] index Index of a surface
   * @return void
   * @relation
   * plugin, wayland
   */
  void DisposeSurface(int64_t index);

  typedef std::map<int64_t, std::unique_ptr<CompositorSurface>> surface_array_t;
  surface_array_t m_comp_surf;

  /**
   * @brief Get a surface context of a compositor surface plugin
   * @param[in] index Index of a surface
   * @return void*
   * @retval a surface context
   * @relation
   * plugin, wayland
   */
  void* GetSurfaceContext(int64_t index);
#endif

#ifdef ENABLE_PLUGIN_COMP_REGION
  /**
   * @brief Clear a region of a subsurface
   * @param[in] type Type of a region
   * @return void
   * @relation
   * wayland
   */
  void ClearRegion(const std::string& type) const;

  /**
   * @brief Set a region of a subsurface
   * @param[in] type Type of a region
   * @param[in] regions Regions of subsurfaces
   * @return void
   * @relation
   * wayland
   */
  void SetRegion(
      const std::string& type,
      const std::vector<CompositorRegionPlugin::REGION_T>& regions) const;
#endif

#if BUILD_COMPOSITOR
  /**
   * @brief Register a plugin-provided @c ICompositorSurface for a
   * platform-view identifier. Forwards to the active backend's compositor.
   *
   * Called by plugins that implement a platform view and want to render
   * into a backing store under the compositor API. Must be paired with
   * @c UnregisterCompositorSurface. Passing a null @p surface unregisters.
   */
  void RegisterCompositorSurface(FlutterPlatformViewIdentifier id,
                                 std::shared_ptr<ICompositorSurface> surface);

  void UnregisterCompositorSurface(FlutterPlatformViewIdentifier id);

  /**
   * @brief Notify a registered surface of a size change.
   *
   * Invoked by @c PlatformViewsHandler when the Dart widget resizes.
   * No-op when @p id is not registered.
   */
  void ResizeCompositorSurface(FlutterPlatformViewIdentifier id,
                               int32_t width,
                               int32_t height);
#endif

  FML_DISALLOW_COPY_AND_ASSIGN(FlutterView);

 private:
#if BUILD_BACKEND_HEADLESS_EGL
  std::shared_ptr<HeadlessBackend> m_backend;
#elif BUILD_BACKEND_DRM_KMS_EGL
  std::shared_ptr<DrmBackend> m_backend{};
#elif BUILD_BACKEND_DRM_KMS_VULKAN
  std::shared_ptr<VulkanDrmBackend> m_backend{};
#elif BUILD_BACKEND_SOFTWARE
  std::shared_ptr<SoftwareBackend> m_backend;
#elif BUILD_BACKEND_WAYLAND_EGL
  std::shared_ptr<WaylandEglBackend> m_backend;
#elif BUILD_BACKEND_WAYLAND_VULKAN
  std::shared_ptr<WaylandVulkanBackend> m_backend;
#else
#error "no Flutter backend selected (see forward-decl block above)"
#endif
  std::shared_ptr<IDisplay> m_display;
  // Default-null on non-Wayland backends (only assigned under the
  // BUILD_BACKEND_WAYLAND_* gate in flutter_view.cc::Initialize).
  std::shared_ptr<WaylandWindow> m_wayland_window;
  std::shared_ptr<Engine> m_flutter_engine;
  std::shared_ptr<AccessibilityTree> m_accessibility_tree;
  const Configuration::Config m_config;
  std::shared_ptr<PlatformChannel> m_platform_channel;
  size_t m_index;

  std::unique_ptr<FlutterDesktopViewControllerState> m_state;

  // Temporary owner of FlutterDesktopEngineState between FlutterView
  // construction (where engine_state is allocated and populated) and
  // Initialize() (where it's moved into m_flutter_engine via
  // Engine::TakeEngineState). After the move this is null; lifetime
  // belongs to m_flutter_engine so engine_state outlives
  // FlutterEngineDeinitialize. m_state->engine_state is a non-owning
  // raw pointer to the same target throughout.
  std::unique_ptr<FlutterDesktopEngineState> m_pending_engine_state;

  static void RegisterPlugins(FlutterDesktopEngineRef engine);
};
