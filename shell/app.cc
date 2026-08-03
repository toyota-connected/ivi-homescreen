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
#if BUILD_BACKEND_HEADLESS_VULKAN
#include "vk_export_bridge_loader.h"
#endif

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

#include <unistd.h>

#include "config/common.h"

#include "main_loop_waker.h"
#include "timer.h"
#include "view/flutter_view.h"

#if BUILD_WATCHDOG
#include "watchdog/watchdog.h"
#endif

#include <chrono>

#include "asio/io_context.hpp"
#include "asio/posix/stream_descriptor.hpp"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"

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

// The device-context key for a view: the set of views sharing one display.
// All Wayland views share the single compositor connection (device-less); each
// DRM GPU / drm-dumb device is its own context. The output layer (multiple
// connectors per GPU, multiple wl_outputs per connection) is resolved within a
// context, not here.
std::string ContextKey(const backend::BackendRegistry& reg,
                       const Configuration::Config& config) {
  const std::string key = ResolveKeyForConfig(reg, config);
  if (key.rfind("wayland-", 0) == 0) {
    return "wayland";
  }
  return key + "|" + config.view.drm_device.value_or("");
}

}  // namespace

std::vector<std::shared_ptr<IDisplay>> App::BuildDisplays(
    const std::vector<Configuration::Config>& configs) {
  // App is self-contained: ensure the runtime registry has an active backend
  // (registering the compiled-in backends + resolving from configs on first
  // use, or respecting one main() set explicitly) before creating any display.
  // The active backend backs the first bundle and process-level queries; each
  // view independently resolves its own backend (ResolveKeyForConfig) below.
  auto& reg = backend::BackendRegistry::Instance();
  if (!EnsureActiveBackend(reg, configs)) {
    ihs::log::critical("[App] no usable backend available; aborting");
    std::exit(EXIT_FAILURE);
  }

  // Group config indices by device-context, preserving first-seen order so the
  // primary bundle's display stays first (it backs process-level wiring).
  std::vector<std::string> order;
  std::unordered_map<std::string, std::vector<size_t>> groups;
  for (size_t i = 0; i < configs.size(); ++i) {
    const std::string ck = ContextKey(reg, configs[i]);
    auto it = groups.find(ck);
    if (it == groups.end()) {
      order.push_back(ck);
      groups.emplace(ck, std::vector<size_t>{i});
    } else {
      it->second.push_back(i);
    }
  }

  // Create one display per context from that context's configs (a group
  // member's backend descriptor owns the creation body), and record each view's
  // display so FlutterView is placed on the display for its (backend, device).
  std::vector<std::shared_ptr<IDisplay>> per_config(configs.size());
  for (const std::string& ck : order) {
    const std::vector<size_t>& members = groups[ck];
    std::vector<Configuration::Config> group_configs;
    group_configs.reserve(members.size());
    for (const size_t idx : members) {
      group_configs.push_back(configs[idx]);
    }
    const std::string key = ResolveKeyForConfig(reg, group_configs.front());
    const backend::BackendDescriptor* descriptor = reg.Resolve(key);
    if (descriptor == nullptr) {
      ihs::log::critical("[App] no backend resolved for context '{}'", ck);
      std::exit(EXIT_FAILURE);
    }
    auto display = descriptor->make_display(group_configs);
    m_displays.push_back(display);
    for (const size_t idx : members) {
      per_config[idx] = display;
    }
  }
  return per_config;
}

App::App(const std::vector<Configuration::Config>& configs) {
  IHS_DEBUG("+App::App");
  // One display per device-context; view_display[i] is the display config i
  // runs on (views sharing a context share a display).
  const std::vector<std::shared_ptr<IDisplay>> view_display =
      BuildDisplays(configs);
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
    auto view = std::make_unique<FlutterView>(cfg, index, view_display[index]);
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
  Watchdog::getInstance().start(WATCHDOG_SOURCE_MAIN_THREAD);
  Watchdog::getInstance().start(WATCHDOG_SOURCE_RENDER_THREAD);
  next_pet_ = std::chrono::steady_clock::now();
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

#if BUILD_BACKEND_HEADLESS_VULKAN
  // Bring up the in-process ihs-vk-export bridge last: the headless-vulkan
  // backend registered its export API during display/view construction above,
  // so the module's dlsym(RTLD_DEFAULT) now resolves to a live backend. Inert
  // unless IVI_VK_BRIDGE_SO is set.
  vk_export_bridge_ = std::make_unique<ihs::VkExportBridgeLoader>();
#endif

  IHS_DEBUG("-App::App");
}

App::~App() {
#if BUILD_BACKEND_HEADLESS_VULKAN
  // Stop and unload the bridge before anything else: this clears the backend's
  // raster-thread frame listener and joins the module's IO thread while the
  // backend (owned by the views/displays below) is still alive.
  vk_export_bridge_.reset();
#endif
  for (const auto& display : m_displays) {
    display->StopEvents();
  }
#if BUILD_WATCHDOG
  Watchdog::getInstance().stop(WATCHDOG_SOURCE_RENDER_THREAD);
  Watchdog::getInstance().stop(WATCHDOG_SOURCE_MAIN_THREAD);
#endif
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
  Watchdog::getInstance().pet(WATCHDOG_SOURCE_MAIN_THREAD);

  // Schedule RenderThread pet via callback
  if (next_pet_ <= std::chrono::steady_clock::now()) {
    for (auto const& view : m_views) {
      view->PetWatchdogViaCallback();
    }
    uint64_t interval = Watchdog::getInstance().getTimeoutMs() / 2;
    next_pet_ =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(interval);
  }
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
  // The shared reactor is owned by App and drives every backend. Reactor
  // backends (Wayland) armed their display fds onto it in the ctor
  // (SetEventLoop
  // + StartEvents); self-driving backends (DRM, software) run their own
  // seat/flip threads, so for them the reactor only services the refresh-rate
  // pump (periodic RunTasks) and the shutdown/wake eventfd — the same duties
  // the legacy per-iteration loop carried.
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
    Watchdog::getInstance().pet(WATCHDOG_SOURCE_MAIN_THREAD);
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

#if BUILD_WATCHDOG
  // Watchdog heartbeat: pet the main-thread source on its own cadence so the
  // reactor's idle gaps (no input, needs_periodic=false) never exceed the
  // watchdog timeout. Fires every intervalMs/2, matching the watchdog service
  // check period.
  asio::steady_timer wd_timer(ioc);
  std::function<void(const std::error_code&)> on_wd_timer;
  std::function<void()> arm_wd_timer = [&]() {
    wd_timer.expires_after(
        std::chrono::milliseconds(Watchdog::getInstance().getTimeoutMs() / 2));
    wd_timer.async_wait(on_wd_timer);
  };
  on_wd_timer = [&](const std::error_code& ec) {
    if (ec) {
      return;  // cancelled
    }

    // Pet the dogs
    Watchdog::getInstance().pet(WATCHDOG_SOURCE_MAIN_THREAD);
    for (auto const& view : m_views) {
      view->PetWatchdogViaCallback();
    }

    arm_wd_timer();
  };
  arm_wd_timer();
#endif

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
}
