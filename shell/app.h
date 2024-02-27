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

#include "configuration/configuration.h"
#include "view/flutter_view.h"

class Display;

class WaylandWindow;

class App {
 public:
  explicit App(const std::vector<Configuration::Config>& configs);

  App(const App&) = delete;

  const App& operator=(const App&) = delete;

  /**
   * @brief One frame in the loop
   * @return int
   * @retval Number of dispatched events
   * @relation
   * wayland, flutter
   */
  NODISCARD int Loop() const;

#if defined(BUILD_BACKEND_HEADLESS)
  uint8_t* getViewRenderBuf(int i);
#endif

 private:
  std::shared_ptr<Display> m_wayland_display;
  std::vector<std::unique_ptr<FlutterView>> m_views;
};
