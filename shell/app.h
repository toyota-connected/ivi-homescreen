/*
 * Copyright 2020 Toyota Connected North America
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

#include <EGL/egl.h>
#include <chrono>
#include <memory>
#include <string_view>

#include <unordered_map>
#include "config/common.h"  // ENABLE_DLT — keep before logger.hpp / member decl

#include "configuration/configuration.h"
#include "display/output.h"
#include "logging/logger.hpp"
#include "view/flutter_view.h"

#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"

class IDisplay;

class WaylandWindow;

namespace ihs {
class VkExportBridgeLoader;
}  // namespace ihs

class App final {
 public:
  explicit App(const std::vector<Configuration::Config>& configs);
  ~App();

  App(const App&) = delete;

  const App& operator=(const App&) = delete;

  /**
   * @brief Service one unit of main-thread work (pump view tasks, pace once).
   *
   * Not the production loop — Run() drives the reactor. Retained as a
   * single-iteration entry point for tests and diagnostics.
   *
   * @return int
   * @retval Number of dispatched events
   * @relation flutter
   */
  [[nodiscard]] int Loop() const;

  /**
   * @brief Construct and start one additional view after construction.
   *
   * The constructor builds every view in @p configs back to back, and
   * FlutterView::Initialize() runs the engine -- so by the time it returns,
   * every engine is already competing for the GPU. That is the right shape for
   * a fixed view set, but it cannot express "this view must be up before the
   * next one starts", which is exactly what an OSGi critical bundle is.
   *
   * This is the incremental entry point: it resolves @p config against the
   * displays already built (reusing the one for its device-context, or
   * creating a display when the context is new), constructs the view, and runs
   * its engine -- one view, when the caller says so.
   *
   * Safe to call before or after Run(). Returns nullptr if no backend resolves
   * for the config, which the caller reports as a failed bundle rather than
   * taking the whole process down.
   *
   * @relation flutter, osgi
   */
  // This view's identity: derived from its bundle and unique among live
  // views. Names the view's semantics tree and is handed to the application.
  [[nodiscard]] std::string NameForView(
      const Configuration::Config& config) const;

  FlutterView* AddView(const Configuration::Config& config);

  /**
   * @brief Tear down a view previously returned by AddView().
   *
   * Returns true if @p view was owned by this App. Used when a bundle fails to
   * report ACTIVE within its deadline: a half-started bundle must not be left
   * holding a surface and a vsync registration.
   *
   * @relation flutter, osgi
   */
  bool RemoveView(FlutterView* view);

  /**
   * @brief Run the application until shutdown is requested.
   *
   * Drives the shared asio reactor (a single io_context) on the calling thread
   * for every backend: Wayland connections service their {wl-fd, repeat-fd}
   * event-driven; DRM/software displays self-drive their own threads while the
   * reactor runs the refresh-rate plugin pump (only while a view needs it) and
   * the shutdown/wake eventfd.
   *
   * @return 0 on a clean exit, -1 on fatal error.
   * @relation flutter
   */
  int Run();

 private:
  // Builds one display per device-context across @p configs, appends the
  // distinct displays to m_displays, and returns the per-config assignment
  // (parallel to configs): every view runs on the display for its (backend,
  // device). A homogeneous config set yields a single shared display.
  std::vector<std::shared_ptr<IDisplay>> BuildDisplays(
      const std::vector<Configuration::Config>& configs);

  // Display for @p config's device-context, reusing an existing one when the
  // context already has a display and creating one otherwise. Shared by
  // BuildDisplays and AddView so both place a view on the same display for the
  // same (backend, device) -- two displays on one device would fight over the
  // DRM master.
  std::shared_ptr<IDisplay> DisplayForContext(
      const Configuration::Config& config);

  // Start watching @p display's outputs, if it reports hotplug at all. Called
  // for displays built at construction and for any created later by AddView.
  void WatchDisplayOutputs(const std::shared_ptr<IDisplay>& display);

  // Watches one display's outputs on App's behalf. IOutputListener reports
  // what changed but not which display reported it, and App can drive several
  // -- so one listener per display, each carrying its own display pointer,
  // rather than App implementing the interface once and having to guess.
  class OutputWatch;

  // Re-resolve every view on @p display and act on what changed. Runs on the
  // App reactor thread (the one reading the Wayland fd), not the Flutter
  // platform thread -- anything touching the engine hops via
  // FlutterView::PostToPlatformThread.
  void OnDisplayOutputsChanged(IDisplay* display,
                               homescreen::OutputEvent event,
                               std::string_view output_name);

  // Park a view whose output went away, per its [view.output] on_disconnect.
  static void ApplyOnDisconnect(FlutterView* view);

  // Tell one view's platform views their grant is stale.
  static void RenegotiateView(const FlutterView* view);

  // The displays the App drives. One entry per distinct device-context; a
  // homogeneous config set yields a single shared display.
  std::vector<std::shared_ptr<IDisplay>> m_displays;
  // Context key -> display, so AddView can join a view to the display its
  // device-context already has instead of creating a second one.
  std::unordered_map<std::string, std::shared_ptr<IDisplay>>
      m_display_by_context;
  // True once Run() has started the displays, so a view added later knows to
  // start its display itself rather than waiting for a StartEvents pass that
  // has already happened.
  bool m_displays_started{false};
  std::vector<std::unique_ptr<FlutterView>> m_views;
  // One per display that reports hotplug; cleared before the displays go.
  std::vector<std::unique_ptr<OutputWatch>> m_output_watches;
#if BUILD_WATCHDOG
  mutable std::chrono::steady_clock::time_point next_pet_;
#endif

  // Reductions across the owned displays.
  [[nodiscard]] bool AnyHasRepeatTimer() const;
  [[nodiscard]] double MaxRefreshRate() const;

  // The single shared reactor (the "primary" io_context) that Run() drives on
  // the main thread for every backend. Each Wayland Display connection
  // registers its {wl-fd, repeat-fd} onto it (so all connections stay
  // single-threaded with no per-connection strand); DRM/software displays
  // self-drive their own threads and only use the reactor's refresh-rate pump +
  // shutdown waker. The work guard keeps run() alive across idle gaps.
  asio::io_context primary_ioc_;
  asio::executor_work_guard<asio::io_context::executor_type> primary_work_{
      asio::make_work_guard(primary_ioc_)};

#if BUILD_BACKEND_HEADLESS_VULKAN
  // Optional in-process ihs-vk-export bridge (enabled by IVI_VK_BRIDGE_SO).
  // Constructed at the end of the App ctor — after the headless-vulkan backend
  // has registered its export API — and reset at the start of ~App so the
  // module's raster-thread frame listener is cleared and its IO thread joined
  // while the backend is still alive. Inert unless the env var is set.
  std::unique_ptr<ihs::VkExportBridgeLoader> vk_export_bridge_;
#endif
};
