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

#include <thread>

#include "config/common.h"

#include "timer.h"
#include "view/flutter_view.h"

#include "wayland/display.h"
#if BUILD_BACKEND_DRM_KMS_EGL
#include "display/drm_display.h"
#endif

#if BUILD_BACKEND_HEADLESS_EGL
#include "backend/headless/headless.h"
#endif

namespace {

std::shared_ptr<IDisplay> MakeDisplay(
    const std::vector<Configuration::Config>& configs) {
#if BUILD_BACKEND_DRM_KMS_EGL
  // DRM/KMS does not have a compositor-level display concept. The refresh
  // rate and mode are owned by the backend; the DrmDisplay stub answers
  // queries the shell issues (metrics, cursor activation, event loop) with
  // safe defaults. Backend-side hooks can refine the refresh rate later.
  const auto w = configs[0].view.width.value_or(kDefaultViewWidth);
  const auto h = configs[0].view.height.value_or(kDefaultViewHeight);
  return std::make_shared<DrmDisplay>(static_cast<int32_t>(w),
                                      static_cast<int32_t>(h), 60.0);
#else
  return std::make_shared<Display>(!configs[0].disable_cursor,
                                   configs[0].wayland_event_mask,
                                   configs[0].cursor_theme, configs);
#endif
}

}  // namespace

App::App(const std::vector<Configuration::Config>& configs)
    : m_display(MakeDisplay(configs)) {
  SPDLOG_DEBUG("+App::App");
#if ENABLE_AGL_SHELL_CLIENT
  bool found_view_with_bg = false;
#endif

  size_t index = 0;
  m_views.reserve(configs.size());
  for (auto const& cfg : configs) {
    auto view = std::make_unique<FlutterView>(cfg, index, m_display);
    view->Initialize();
    m_views.emplace_back(std::move(view));
    index++;

#if ENABLE_AGL_SHELL_CLIENT
    if (WaylandWindow::get_window_type(cfg.view.window_type) ==
        WaylandWindow::WINDOW_BG) {
      found_view_with_bg = true;
    }
#endif
  }

#if ENABLE_AGL_SHELL_CLIENT
  // check that if we had a BG type and issue a ready() request for it,
  // otherwise we're going to assume that this is a NORMAL/REGULAR application.
  if (found_view_with_bg)
    static_cast<Display*>(m_display.get())->AglShellDoReady();
#endif

#if BUILD_WATCHDOG
  m_watch_dog = std::make_unique<Watchdog>();
#endif

  m_display->StartEvents();

  SPDLOG_DEBUG("-App::App");
}

App::~App() {
  m_display->StopEvents();
}

int App::Loop() const {
  const auto start_time =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();

  for (auto const& view : m_views) {
    view->RunTasks();
  }

  if (m_display->HasRepeatTimer())
    EventTimer::wait_event();

  const auto end_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();

  const auto elapsed = end_time - start_time;

  const auto frame_time = 1000.0 / m_display->GetMaxRefreshRate();
  if (const auto sleep_time = frame_time - static_cast<double>(elapsed);
      sleep_time > 0) {
#if BUILD_WATCHDOG
    m_watch_dog->pet();
#endif
    std::this_thread::sleep_for(
        std::chrono::duration<double, std::milli>(sleep_time));
  }

  return 0;
}

#if BUILD_BACKEND_HEADLESS_EGL

GLubyte* App::getViewRenderBuf(const int i) const {
  return reinterpret_cast<HeadlessBackend*>(
             m_views[static_cast<unsigned long>(i)]->GetBackend())
      ->getHeadlessBuffer();
}

#endif
