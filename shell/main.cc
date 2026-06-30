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
  // [sentry] is process-level: read it from the --config master file when
  // given, otherwise from the first bundle's config.toml.
  std::string sentry_config_path;
  if (!configs.empty()) {
    const auto& front = configs.front();
    if (front.config_file && !front.config_file->empty()) {
      sentry_config_path = *front.config_file;
    } else if (!front.view.bundle_path.empty()) {
      sentry_config_path =
          front.view.bundle_path + "/" + std::string(kViewConfigToml);
    }
  }
  auto crash_handler = std::make_unique<CrashHandler>(sentry_config_path);
#endif

  // Handle --drm-list-modes[=<path>] as an early exit so the user doesn't need
  // to supply a bundle just to inspect modes. It is dispatched to the active
  // backend's mode lister (DRM lists the scanout connector's modes; software
  // the DRM dumb-sink's, the values valid for IVI_SW_DRM_MODE); backends with
  // no DRM device report it unsupported. Device resolution precedence (first
  // match wins):
  //   1. --drm-list-modes=<path> (explicit attached value)
  //   2. --drm-list-modes <path> (next positional, unless it starts with -)
  //   3. --drm-device <path> or --drm-device=<path> anywhere in argv
  //      (the same flag the launch path consumes)
  //   4. the backend's default (e.g. /dev/dri/card1 for DRM)
  // Step 3 is the fix for `--drm-list-modes --drm-device /dev/dri/cardN`,
  // which previously silently fell through because the next-positional check at
  // step 2 rejects anything starting with `-`.
  bool list_modes_requested = false;
  std::string list_modes_dev;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--drm-list-modes") {
      list_modes_requested = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        list_modes_dev = argv[i + 1];
      }
      continue;
    }
    if (arg.rfind("--drm-list-modes=", 0) == 0) {
      list_modes_requested = true;
      list_modes_dev =
          std::string(arg.substr(std::strlen("--drm-list-modes=")));
      continue;
    }
    if (!list_modes_dev.empty()) {
      continue;
    }
    if (arg == "--drm-device" && i + 1 < argc) {
      list_modes_dev = argv[i + 1];
    } else if (arg.rfind("--drm-device=", 0) == 0) {
      list_modes_dev = std::string(arg.substr(std::strlen("--drm-device=")));
    }
  }
  if (list_modes_requested) {
    auto& reg = backend::BackendRegistry::Instance();
    if (!EnsureActiveBackend(reg, configs)) {
      return EXIT_FAILURE;
    }
    const backend::BackendDescriptor& active = reg.Active();
    if (!active.list_modes) {
      spdlog::critical(
          "[main] the '{}' backend does not support --drm-list-modes",
          active.key);
      return EXIT_FAILURE;
    }
    return active.list_modes(list_modes_dev) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }

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
