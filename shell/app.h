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
#include <memory>

#include "config/common.h"
#include "configuration/configuration.h"
#include "view/flutter_view.h"
#include "watchdog.h"

#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"

class IDisplay;

class WaylandWindow;

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

  // The displays the App drives. One entry per distinct device-context; a
  // homogeneous config set yields a single shared display.
  std::vector<std::shared_ptr<IDisplay>> m_displays;
  std::vector<std::unique_ptr<FlutterView>> m_views;
  std::unique_ptr<Watchdog> m_watch_dog;

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
};
