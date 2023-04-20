// Copyright 2020 Toyota Connected North America
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

#include "app.h"

#include <sstream>
#include <thread>

#include "constants.h"
#include "view/flutter_view.h"
#include "wayland/display.h"

App::App(const std::vector<Configuration::Config>& configs)
    : m_wayland_display(std::make_shared<Display>(!configs[0].disable_cursor,
                                                  configs[0].cursor_theme,
                                                  configs)) {
  DLOG(INFO) << "+App::App";
  bool found_view_with_bg = false;

  size_t index = 0;
  m_views.reserve(configs.size());
  for (auto const& cfg : configs) {
    auto view = std::make_unique<FlutterView>(cfg, index, m_wayland_display);
    view->Initialize();
    m_views.emplace_back(std::move(view));
    index++;

    if (WaylandWindow::get_window_type(cfg.view.window_type) ==
        WaylandWindow::WINDOW_BG)
      found_view_with_bg = true;
  }

  // check that if we had a BG type and issue a ready() request for it,
  // otherwise we're going to assume that this is a NORMAL/REGULAR application.
  if (found_view_with_bg)
    m_wayland_display->AglShellDoReady();

  DLOG(INFO) << "-App::App";
}

int App::Loop() {
  auto start_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();

  auto ret = m_wayland_display->PollEvents();

  for (auto const& view : m_views) {
    view->RunTasks();
  }

  if (m_wayland_display->m_repeat_timer)
    m_wayland_display->m_repeat_timer->wait_event();

  auto end_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();

  auto elapsed = end_time - start_time;

  auto sleep_time = 16 - elapsed;

  if (sleep_time > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
  }

  for (auto const& i : m_views) {
    i->DrawFps(end_time);
  }

  return ret;
}
