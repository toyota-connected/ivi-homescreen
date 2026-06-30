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

#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN
#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"
#endif

class IDisplay;

class WaylandWindow;

class App final {
 public:
  explicit App(const std::vector<Configuration::Config>& configs);
  ~App();

  App(const App&) = delete;

  const App& operator=(const App&) = delete;

  /**
   * @brief One frame in the loop
   * @return int
   * @retval Number of dispatched events
   * @relation
   * wayland, flutter
   */
  [[nodiscard]] int Loop() const;

  /**
   * @brief Run the application until shutdown is requested.
   *
   * On the Wayland backend this runs the display reactor (a single asio
   * io_context) on the calling thread: the Wayland fd, keyboard repeat timerfd
   * and the shutdown waker are all serviced event-driven, with periodic plugin
   * pumping paced by a refresh-rate timer only while a view needs it. Other
   * backends fall back to the Loop()-per-iteration model.
   *
   * @return 0 on a clean exit, -1 on fatal error.
   * @relation wayland, flutter
   */
  int Run();

 private:
  // The displays the App drives. One entry per distinct (backend, device); a
  // homogeneous config set yields a single shared display.
  std::vector<std::shared_ptr<IDisplay>> m_displays;
  std::vector<std::unique_ptr<FlutterView>> m_views;
  std::unique_ptr<Watchdog> m_watch_dog;

  // Reductions across the owned displays.
  [[nodiscard]] bool AnyHasRepeatTimer() const;
  [[nodiscard]] double MaxRefreshRate() const;

#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN
  // The single shared reactor (the "primary" io_context). Run() runs it on the
  // main thread; every Wayland Display connection registers its {wl-fd,
  // repeat-fd} onto it, so all connections stay single-threaded with no
  // per-connection strand. The work guard keeps run() alive across idle gaps.
  asio::io_context primary_ioc_;
  asio::executor_work_guard<asio::io_context::executor_type> primary_work_{
      asio::make_work_guard(primary_ioc_)};
#endif
};
