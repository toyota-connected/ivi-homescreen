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

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <string_view>
#include <system_error>

#include <unistd.h>

#include "config/common.h"

#include "main_loop_waker.h"
#include "timer.h"
#include "view/flutter_view.h"

#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN
#include <chrono>

#include "asio/io_context.hpp"
#include "asio/posix/stream_descriptor.hpp"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"
#endif

#include "shutdown_flag.h"

// Process-wide shutdown flag. Declared in shutdown_flag.h and DEFINED here (in
// the shell library) rather than in main.cc so the library is self-contained:
// main.cc's signal handler clears it, and the reactor / legacy loop re-check it
// (woken via the MainLoopWaker eventfd).
volatile sig_atomic_t running = 1;

#include "backend/backend_registry.h"
#include "backend/register_backends.h"

#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN
#include "wayland/display.h"
#include "wayland/window.h"
#endif

namespace {

// Idle wakeup cadence when the loop has no periodic work pending. Acts as a
// liveness heartbeat / safety net in case a wake source is ever missed; the
// loop is normally woken on demand by MainLoopWaker.
constexpr int kIdleHeartbeatMs = 1000;

std::shared_ptr<IDisplay> MakeDisplay(
    const std::vector<Configuration::Config>& configs) {
  // App is self-contained: ensure the runtime registry has an active backend
  // (registering the compiled-in backends + resolving from configs on first
  // use, or respecting one main() set explicitly) before creating its display.
  // The active descriptor owns the display-creation body that used to live here
  // as an #if chain (the bodies now live in backend/register_backends.cc).
  // Fail-fast: a null backend would crash the engine init.
  auto& reg = backend::BackendRegistry::Instance();
  if (!EnsureActiveBackend(reg, configs)) {
    spdlog::critical("[App] no usable backend available; aborting");
    std::exit(EXIT_FAILURE);
  }
  return reg.Active().make_display(configs);
}

}  // namespace

App::App(const std::vector<Configuration::Config>& configs)
    : m_displays{MakeDisplay(configs)} {
  SPDLOG_DEBUG("+App::App");
// AGL Shell needs a Wayland backend — its WaylandWindow / Display
// usage is meaningless on DRM / software. ENABLE_AGL_SHELL_CLIENT
// defaults ON regardless of backend, so combine with a Wayland-backend
// gate here.
#if ENABLE_AGL_SHELL_CLIENT && \
    (BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN)
  bool found_view_with_bg = false;
#endif

  size_t index = 0;
  m_views.reserve(configs.size());
  for (auto const& cfg : configs) {
    auto view = std::make_unique<FlutterView>(cfg, index, m_displays.front());
    view->Initialize();
    m_views.emplace_back(std::move(view));
    index++;

// AGL Shell needs a Wayland backend — its WaylandWindow / Display
// usage is meaningless on DRM / software. ENABLE_AGL_SHELL_CLIENT
// defaults ON regardless of backend, so combine with a Wayland-backend
// gate here.
#if ENABLE_AGL_SHELL_CLIENT && \
    (BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN)
    if (WaylandWindow::get_window_type(cfg.view.window_type) ==
        WaylandWindow::WINDOW_BG) {
      found_view_with_bg = true;
    }
#endif
  }

// AGL Shell needs a Wayland backend — its WaylandWindow / Display
// usage is meaningless on DRM / software. ENABLE_AGL_SHELL_CLIENT
// defaults ON regardless of backend, so combine with a Wayland-backend
// gate here.
#if ENABLE_AGL_SHELL_CLIENT && \
    (BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN)
  // check that if we had a BG type and issue a ready() request for it,
  // otherwise we're going to assume that this is a NORMAL/REGULAR application.
  // OnClientReady maps to agl_shell.ready() on AglShell and is a no-op on the
  // other shells. Null-check the cast: in a multi-backend binary the active
  // display may be DRM/software (not a Wayland Display).
  if (found_view_with_bg) {
    for (const auto& display : m_displays) {
      if (auto* d = dynamic_cast<Display*>(display.get())) {
        d->ActiveShell().OnClientReady();
      }
    }
  }
#endif

#if BUILD_WATCHDOG
  m_watch_dog = std::make_unique<Watchdog>();
#endif

#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN
  // Wire the App-owned shared reactor onto the Wayland connection before it
  // arms its display fd. Multiple connections would each register onto the same
  // primary_ioc_.
  for (const auto& display : m_displays) {
    if (auto* d = dynamic_cast<Display*>(display.get())) {
      d->SetEventLoop(primary_ioc_);
    }
  }
#endif

  for (const auto& display : m_displays) {
    display->StartEvents();
  }

  SPDLOG_DEBUG("-App::App");
}

App::~App() {
  for (const auto& display : m_displays) {
    display->StopEvents();
  }
}

bool App::AnyHasRepeatTimer() const {
  return std::any_of(
      m_displays.begin(), m_displays.end(),
      [](const auto& display) { return display->HasRepeatTimer(); });
}

double App::MaxRefreshRate() const {
  double hz = 0.0;
  for (const auto& display : m_displays) {
    const double r = display->GetMaxRefreshRate();
    if (r > hz) {
      hz = r;
    }
  }
  return hz;
}

int App::Loop() const {
  const auto start_time =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();

  for (auto const& view : m_views) {
    view->RunTasks();
  }

  if (AnyHasRepeatTimer())
    EventTimer::wait_event();

#if BUILD_WATCHDOG
  m_watch_dog->pet();
#endif

  // Frame production is driven entirely by each backend's own vsync source
  // (wp_presentation feedback, KMS vblank, …) on a separate thread — App::Loop
  // does not render. Its only periodic duties are pumping the Wayland
  // key-repeat timer and any compositor-surface plugins. When neither is
  // active the loop has nothing to do until an input thread coalesces a
  // pointer event (or shutdown is requested), so it blocks on the waker
  // instead of spinning at the refresh rate — letting the CPU reach deep
  // idle on a static screen.
  bool needs_periodic = AnyHasRepeatTimer();
  for (auto const& view : m_views) {
    if (view->NeedsPeriodicPump()) {
      needs_periodic = true;
      break;
    }
  }

  int timeout_ms;
  if (needs_periodic) {
    // Pace at the display refresh rate, minus the work already done this
    // iteration. Some compositors advertise refresh=0 for virtual outputs;
    // fall back to 60 Hz so the timeout stays finite.
    const auto end_time =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const auto elapsed = end_time - start_time;
    const double refresh_hz = MaxRefreshRate();
    const double frame_time =
        refresh_hz > 0.0 ? 1000.0 / refresh_hz : 1000.0 / 60.0;
    const double remaining = frame_time - static_cast<double>(elapsed);
    timeout_ms = remaining > 0.0 ? static_cast<int>(remaining) : 0;
  } else {
    // Idle: block until woken by input or shutdown. The finite heartbeat
    // (rather than an infinite block) bounds the cost of any wake source we
    // might have missed to one extra wakeup per second — a self-healing
    // safety net that still cuts idle wakeups by ~98% versus per-frame.
    timeout_ms = kIdleHeartbeatMs;
  }

  MainLoopWaker::instance().Wait(timeout_ms);

  return 0;
}

int App::Run() {
#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN
  // The shared reactor is owned by App; the Wayland connection(s) already armed
  // their fds onto it (App ctor: SetEventLoop + StartEvents).
  asio::io_context& ioc = primary_ioc_;

  // Refresh-rate frame interval (fallback 60 Hz for virtual outputs that
  // advertise refresh=0).
  auto frame_interval = [this]() -> std::chrono::nanoseconds {
    const double hz = MaxRefreshRate();
    const double ms = hz > 0.0 ? 1000.0 / hz : 1000.0 / 60.0;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double, std::milli>(ms));
  };
  auto needs_periodic = [this]() {
    for (auto const& view : m_views) {
      if (view->NeedsPeriodicPump()) {
        return true;
      }
    }
    return false;
  };
  // One unit of main-thread work: pump compositor-surface plugins and flush
  // coalesced pointer events, then pet the watchdog. Frame production itself is
  // driven by the backend's own vsync source on a separate thread.
  auto service = [this]() {
    for (auto const& view : m_views) {
      view->RunTasks();
    }
#if BUILD_WATCHDOG
    m_watch_dog->pet();
#endif
  };

  // Recurring pump timer: re-arms at the refresh rate only while a view needs
  // periodic servicing; otherwise the reactor idles until the next fd event.
  asio::steady_timer pump(ioc);
  bool pump_armed = false;
  std::function<void(const std::error_code&)> on_pump;
  std::function<void()> arm_pump = [&]() {
    if (pump_armed) {
      return;
    }
    pump_armed = true;
    pump.expires_after(frame_interval());
    pump.async_wait(on_pump);
  };
  on_pump = [&](const std::error_code& ec) {
    pump_armed = false;
    if (ec) {
      return;  // cancelled
    }
    service();
    if (needs_periodic()) {
      arm_pump();
    }
  };

  // Shutdown + on-demand wake: the MainLoopWaker eventfd is written by the
  // signal handler (shutdown) and by Engine input coalescing / view requests
  // (a task needs pumping while otherwise idle). Registered non-owning — the
  // eventfd belongs to the MainLoopWaker singleton, so release() it on exit.
  asio::posix::stream_descriptor waker(ioc, MainLoopWaker::instance().fd());
  std::function<void(const std::error_code&)> on_wake;
  std::function<void()> arm_wake = [&]() {
    waker.async_wait(asio::posix::stream_descriptor::wait_read, on_wake);
  };
  on_wake = [&](const std::error_code& ec) {
    if (ec) {
      return;  // cancelled
    }
    // Drain the eventfd counter.
    uint64_t drained = 0;
    while (::read(waker.native_handle(), &drained, sizeof(drained)) ==
           static_cast<ssize_t>(sizeof(drained))) {
    }
    if (!running) {
      // App owns the reactor: release the work guard and stop it so run()
      // returns. Each Display detaches its own fds in StopEvents() / ~Display.
      primary_work_.reset();
      primary_ioc_.stop();
      return;  // do not re-arm
    }
    service();
    if (needs_periodic()) {
      arm_pump();
    }
    arm_wake();
  };

  // Initial servicing + arm the watches, then block the main thread on the
  // reactor until shutdown stops it.
  asio::post(ioc, [&]() {
    service();
    if (needs_periodic()) {
      arm_pump();
    }
  });
  arm_wake();

  primary_ioc_.run();

  // Detach from the shared eventfd so asio does not close it.
  std::error_code ec;
  waker.cancel(ec);
  waker.release();
  return 0;
#else
  int ret = 0;
  while (running && ret != -1) {
    ret = Loop();
  }
  return ret;
#endif
}
