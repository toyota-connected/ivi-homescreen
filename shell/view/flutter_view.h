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

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "configuration/configuration.h"
#include "display/idisplay.h"
#include "flutter/fml/macros.h"
#include "flutter_desktop_view_controller_state.h"
#if BUILD_ACCESSIBILITY
#include "shell/accessibility/accessibility_tree.h"
#endif
#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN
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
class PlatformViewRegistry;
namespace flutter {
class KeyEventHandler;
}
// m_backend is a polymorphic std::shared_ptr<Backend> (built by
// Backend::Create); concrete backend types are no longer named here.
class Backend;
#ifdef ENABLE_PLUGIN_COMP_SURF
class CompositorSurface;
#endif

class TaskRunner;

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
  [[nodiscard]] Backend* GetBackend() const { return m_backend.get(); }

  // Re-send FlutterEngineDisplay metadata (physical px + effective pixel
  // ratio) to a running engine. Called by WaylandWindow::ApplyScale when the
  // surface scale changes; safe no-op before the engine runs.
  void UpdateDisplayMetadata() const;

  /**
   * @brief Get an index of flutter views
   * @return uint64_t
   * @retval index
   * @relation
   * internal
   */
  [[nodiscard]] uint64_t GetIndex() const { return m_index; }

  /**
   * @brief The output this view is presenting on, by name.
   *
   * What the view is actually on, which is not always what its config asked
   * for -- a backend falls back to its own pick when no [view.output]
   * constraint resolves. A hotplug listener re-resolves the constraint and
   * compares it with this to decide whether anything has to move.
   *
   * @return the output name in the form OutputInfo::name carries (e.g.
   *         "DSI-1"); nullopt when neither the backend nor the view can name
   *         one -- the software backend, or a Wayland output whose first
   *         wl_output.done has not landed.
   * @relation
   * internal
   */
  [[nodiscard]] std::optional<std::string> BoundOutput() const;

  /**
   * @brief The engine's platform view registry, or nullptr before Initialize().
   *
   * The registry hangs off the engine state, which the view can reach and an
   * outside caller cannot (m_flutter_engine is private). App needs it to
   * dispatch per-view plugin callbacks such as renegotiate.
   *
   * Platform thread only, as PlatformViewRegistry requires.
   *
   * @relation
   * internal
   */
  [[nodiscard]] PlatformViewRegistry* GetPlatformViewRegistry() const;

  /**
   * @brief The config this view was built from.
   *
   * The [view.output] constraint lives here, and re-resolving it is how a
   * hotplug listener decides whether the view still belongs where it is.
   *
   * @relation
   * internal
   */
  [[nodiscard]] const Configuration::Config& GetConfig() const {
    return m_config;
  }

  /**
   * @brief Run @p fn on the Flutter platform thread.
   *
   * Output events reach App on its own reactor thread -- the one reading the
   * Wayland fd -- and PlatformViewRegistry may only be touched on the platform
   * thread, which TaskRunner owns separately. This is the hop between them.
   *
   * @return false if the engine is not running, in which case @p fn is not
   *         called and never will be.
   * @relation
   * internal
   */
  bool PostToPlatformThread(std::function<void()> fn) const;

  /**
   * @brief Forget the output this view was placed on, because it is gone.
   *
   * Wayland only, and only where the view recorded its own placement: the DRM
   * binding comes from the backend, which corrects itself when it re-modesets.
   * Without this the record outlives the output, and a replug of the same name
   * looks like "already there" -- so the view would never be told to
   * renegotiate against the new one.
   *
   * @relation
   * internal
   */
  void NoteOutputLost() { m_wayland_bound_output.reset(); }

  /**
   * @brief Park the view: tell its platform views to stop producing.
   *
   * For a view whose output went away. The engine keeps running and the
   * surface is kept, so returning is immediate -- what stops is the plugin
   * side, which is where the cost of an unseen view actually is: a camera or
   * video plugin holding a decoder busy for a display nobody can look at.
   *
   * Frame production stops too, by withholding the vsync baton rather than by
   * tearing the source down: Flutter asked for a frame and gets no answer, so
   * the UI thread, rasterizer, GPU and scanout all go quiet, and unparking
   * hands back the baton it was holding. That is reversible, which
   * StopVsyncMonitor is not.
   *
   * Idempotent: suspending a suspended view does nothing, so repeated output
   * events do not re-notify plugins.
   *
   * @relation
   * internal
   */
  void Suspend();

  /**
   * @brief Unpark the view; the inverse of Suspend().
   * @relation
   * internal
   */
  void Resume();

 private:
  // The one place the parked state changes: parks/unparks frame production and
  // then tells the plugins. Public entry points are Suspend()/Resume().
  void SetSuspended(bool suspended);

 public:
  [[nodiscard]] bool IsSuspended() const { return m_suspended; }

  /// This view's platform task runner, or nullptr before Initialize(). Exposed
  /// for the OSGi host, which pins a critical bundle's engine thread to the
  /// core its manifest names -- the thread belongs to the runner, and nothing
  /// else outside the view can reach it.
  [[nodiscard]] TaskRunner* GetPlatformTaskRunner() const;

  /**
   * @brief Get pointer to Display object
   * @return Display*
   * @relation
   * internal
   */
#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN
  [[nodiscard]] Display* GetDisplay() const;
#endif

  /**
   * @brief Get the view's display as the backend-agnostic IDisplay.
   * @return IDisplay* — set at construction (never null); valid on every
   *         backend (Wayland Display, DrmDisplay, SoftwareDisplay). Use this
   *         for backend-neutral display calls (e.g. ActivateSystemCursor)
   *         instead of reaching through a concrete window type.
   * @relation
   * internal
   */
  [[nodiscard]] IDisplay* GetIDisplay() const { return m_display.get(); }

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

#if BUILD_WATCHDOG
  void PetWatchdogViaCallback();
#endif

  FML_DISALLOW_COPY_AND_ASSIGN(FlutterView);

 private:
  // One polymorphic Backend, built by Backend::Create(). Backend-specific
  // operations are reached via dynamic_cast where still needed.
  std::shared_ptr<Backend> m_backend;
  std::shared_ptr<IDisplay> m_display;
  // Default-null on non-Wayland backends (only assigned under the
  // BUILD_BACKEND_WAYLAND_* gate in flutter_view.cc::Initialize).
  std::shared_ptr<WaylandWindow> m_wayland_window;
  std::shared_ptr<Engine> m_flutter_engine;
#if BUILD_ACCESSIBILITY
  std::shared_ptr<AccessibilityTree> m_accessibility_tree;
#endif
  const Configuration::Config m_config;
  std::shared_ptr<PlatformChannel> m_platform_channel;
  size_t m_index;

  std::unique_ptr<FlutterDesktopViewControllerState> m_state;

  // Non-owning pointer to the KeyEventHandler in
  // m_state->keyboard_hook_handlers[0].
  // Stored at construction so that Initialize() can call
  // SetEngine/SetTextInputPlugin without a dynamic_cast + assert
  // (important for release builds)
  flutter::KeyEventHandler* m_key_event_handler{};

  uint64_t m_pointer_events{};

  // The Wayland output this view was placed on, by name -- taken from the
  // index actually used, so it is set whether the placement came from
  // [view.output], from wl_output_index, or from the default. The DRM side is
  // not mirrored here: DrmBackend knows which connector it programmed, so
  // BoundOutput() asks it rather than keeping a second copy that could
  // disagree.
  std::optional<std::string> m_wayland_bound_output;

  // Parked: the view's output went away and its platform views were told to
  // stop producing. Not a teardown -- the engine and surface are untouched.
  bool m_suspended = false;

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
