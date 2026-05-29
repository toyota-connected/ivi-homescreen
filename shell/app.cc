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
#include "logging/logging.h"

#include <cstdlib>
#include <string_view>
#include <thread>

#include "config/common.h"

#include "timer.h"
#include "view/flutter_view.h"

#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN || \
    BUILD_BACKEND_HEADLESS_EGL
#include "wayland/display.h"
#include "wayland/window.h"
#endif
#if BUILD_BACKEND_DRM_KMS_EGL
#include "display/drm_display.h"
#endif

#if BUILD_BACKEND_SOFTWARE
#include "display/software_display.h"
#if BUILD_SOFTWARE_INPUT_LIBINPUT
#include "backend/software/input/software_seat.h"
#endif
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
#elif BUILD_BACKEND_SOFTWARE
  // No compositor, no Wayland, no DRM — just an IDisplay that owns
  // (a) a refresh-rate denominator for App::Loop and (b) an optional
  // libinput-backed seat. 60 Hz default; a sink with a real vblank
  // can override later.
  const auto w = configs[0].view.width.value_or(kDefaultViewWidth);
  const auto h = configs[0].view.height.value_or(kDefaultViewHeight);
  auto display = std::make_shared<SoftwareDisplay>(
      static_cast<int32_t>(w), static_cast<int32_t>(h), 60.0);
#if BUILD_SOFTWARE_INPUT_LIBINPUT
  // IVI_SW_INPUT controls whether the seat is wired:
  //   "none"     — skip; engine runs without input (CI default).
  //   anything else (including unset / "libinput" / "auto") — wire
  //   the libinput seat.
  // Default-wire matches operator expectations on device targets and
  // is a no-op on CI hosts that lack /dev/input/event* anyway.
  const char* mode = std::getenv("IVI_SW_INPUT");
  const bool want_input = mode == nullptr || std::string_view(mode) != "none";
  if (want_input) {
    display->SetSeat(std::make_unique<homescreen::SoftwareSeat>(
        static_cast<int32_t>(w), static_cast<int32_t>(h)));
  } else {
    spdlog::info("[SoftwareBackend] IVI_SW_INPUT=none — no input seat");
  }
#endif
  return display;
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
// AGL Shell needs a Wayland backend — its WaylandWindow / Display
// usage is meaningless on DRM / software. ENABLE_AGL_SHELL_CLIENT
// defaults ON in waypp's CMake regardless of backend, so combine
// with a Wayland-backend gate here.
#if ENABLE_AGL_SHELL_CLIENT &&                                    \
    (BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN || \
     BUILD_BACKEND_HEADLESS_EGL)
  bool found_view_with_bg = false;
#endif

  size_t index = 0;
  m_views.reserve(configs.size());
  for (auto const& cfg : configs) {
    auto view = std::make_unique<FlutterView>(cfg, index, m_display);
    view->Initialize();
    m_views.emplace_back(std::move(view));
    index++;

// AGL Shell needs a Wayland backend — its WaylandWindow / Display
// usage is meaningless on DRM / software. ENABLE_AGL_SHELL_CLIENT
// defaults ON in waypp's CMake regardless of backend, so combine
// with a Wayland-backend gate here.
#if ENABLE_AGL_SHELL_CLIENT &&                                    \
    (BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN || \
     BUILD_BACKEND_HEADLESS_EGL)
    if (WaylandWindow::get_window_type(cfg.view.window_type) ==
        WaylandWindow::WINDOW_BG) {
      found_view_with_bg = true;
    }
#endif
  }

// AGL Shell needs a Wayland backend — its WaylandWindow / Display
// usage is meaningless on DRM / software. ENABLE_AGL_SHELL_CLIENT
// defaults ON in waypp's CMake regardless of backend, so combine
// with a Wayland-backend gate here.
#if ENABLE_AGL_SHELL_CLIENT &&                                    \
    (BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN || \
     BUILD_BACKEND_HEADLESS_EGL)
  // check that if we had a BG type and issue a ready() request for it,
  // otherwise we're going to assume that this is a NORMAL/REGULAR application.
  if (found_view_with_bg)
    dynamic_cast<Display*>(m_display.get())->AglShellDoReady();
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

  // Some compositors (e.g. tinywlr, virtual outputs) advertise refresh=0
  // for their mode. Without a fallback, 1000.0 / 0 = +inf and the
  // sleep_for below blocks the main thread forever — pointer events
  // queued by the Wayland event thread never get flushed to Flutter.
  const double refresh_hz = m_display->GetMaxRefreshRate();
  const auto frame_time =
      refresh_hz > 0.0 ? 1000.0 / refresh_hz : 1000.0 / 60.0;
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
