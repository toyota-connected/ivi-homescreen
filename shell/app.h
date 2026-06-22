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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "configuration/configuration.h"
#include "view/flutter_view.h"
#include "watchdog.h"

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

#if BUILD_BACKEND_HEADLESS_SOFTWARE
  // Returns a snapshot of the most recent rendered frame from the MemorySink.
  // row_bytes and height receive the buffer geometry; both are 0 before the
  // first frame presents. The returned vector owns its copy of the pixels.
  std::vector<uint8_t> getViewRenderBuf(int i,
                                        size_t* row_bytes,
                                        size_t* height) const;
#endif

 private:
  std::shared_ptr<IDisplay> m_display;
  std::vector<std::unique_ptr<FlutterView>> m_views;
  std::unique_ptr<Watchdog> m_watch_dog;
};
