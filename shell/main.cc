// Copyright 2020 Toyota Connected North America
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

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "config/common.h"

#include "app.h"
#include "backend/register_backends.h"
#include "configuration/configuration.h"
#include "logging/logging.h"
#include "main_loop_waker.h"

#if BUILD_CRASH_HANDLER
#include "crash_handler.h"
#endif

#include "shutdown_flag.h"

std::unique_ptr<Logging> gLogger;

namespace {

// Async-signal-safe: just flip the running flag. Everything else (logging,
// resource teardown) happens on the main thread when the loop falls through
// and the App / backend destructors run. Calling exit() here would skip
// stack unwinding and strand the console: the DRM backend would never
// restore the saved CRTC, and DrmSeat would never restore the VT keyboard
// mode from K_OFF — leaving the TTY both blank and deaf to keystrokes.
extern "C" void HandleShutdownSignal(int /*sig*/) {
  running = 0;
  // Break the main loop out of an idle block so it re-checks `running`
  // immediately instead of waiting for the next heartbeat. write() to the
  // waker eventfd is async-signal-safe.
  MainLoopWaker::SignalWake();
}

void InstallShutdownHandlers() {
  struct sigaction sa{};
  sa.sa_handler = &HandleShutdownSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  for (const int sig : {SIGINT, SIGTERM, SIGHUP}) {
    sigaction(sig, &sa, nullptr);
  }
}

}  // namespace

/**
 * @brief Main function
 * @param[in] argc Number of arguments
 * @param[in] argv Arguments passed to the program
 * @return int
 * @retval 0 Normal end
 * @retval Non-zero Abnormal end
 * @relation
 * wayland, flutter
 */
int main(const int argc, char** argv) {
  gLogger = std::make_unique<Logging>();
  const auto configs = Configuration::ParseArgcArgv(argc, argv);
  assert(!configs.empty());

#if BUILD_CRASH_HANDLER
  std::string first_bundle_path;
  if (!configs.empty()) {
    first_bundle_path = configs.front().view.bundle_path;
  }
  auto crash_handler = std::make_unique<CrashHandler>(first_bundle_path);
#endif

  // Construct the App. It populates the runtime backend registry and resolves
  // the active backend from its configs on first display creation, so it is
  // self-contained (e.g. the app unit test constructs App without main()).
  App app(configs);

  // Construct the waker (publishing its eventfd) before installing the
  // signal handlers, so HandleShutdownSignal's async-signal-safe wake always
  // has a valid fd to write to.
  (void)MainLoopWaker::instance();
  InstallShutdownHandlers();

  // run the application — the active backend's loop mode (resolved by App)
  // selects the reactor vs legacy dispatch.
  int ret = 0;
  auto& reg = backend::BackendRegistry::Instance();
  if (reg.Active().loop_mode == backend::LoopMode::kReactor) {
    // Reactor backends (Wayland) run their display reactor on this (main)
    // thread; Run() blocks until the shutdown signal stops the io_context.
    ret = app.Run();
  } else {
    // Legacy backends (DRM, software) spin the per-iteration loop.
    while (running && ret != -1) {
      ret = app.Loop();
    }
  }
  (void)ret;

  gLogger.reset();

#if BUILD_CRASH_HANDLER
  (void)crash_handler.release();
#endif

  return EXIT_SUCCESS;
}
