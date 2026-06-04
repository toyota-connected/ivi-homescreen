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

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "config/common.h"

#include "app.h"
#include "configuration/configuration.h"
#include "logging/logging.h"
#include "main_loop_waker.h"

#if BUILD_BACKEND_DRM_KMS_EGL
#include "backend/drm_kms_egl/drm_backend.h"
#endif

#if BUILD_BACKEND_SOFTWARE
#include "backend/software/drm_dumb_sink.h"
#endif

#if BUILD_CRASH_HANDLER
#include "crash_handler.h"
#endif

volatile sig_atomic_t running = 1;

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
#if BUILD_CRASH_HANDLER
  auto crash_handler = std::make_unique<CrashHandler>();
#endif

#if BUILD_BACKEND_DRM_KMS_EGL || BUILD_BACKEND_SOFTWARE
  // Handle --drm-list-modes[=<path>] before the main config parse so the
  // user doesn't need to supply a bundle path just to inspect modes. On
  // software builds this lists the DRM dumb-sink's modes (the values valid
  // for IVI_SW_DRM_MODE); on drm-kms-egl it lists the scanout connector's.
  // Device resolution precedence (first match wins):
  //   1. --drm-list-modes=<path> (explicit attached value)
  //   2. --drm-list-modes <path> (next positional, unless it starts with -)
  //   3. --drm-device <path> or --drm-device=<path> anywhere in argv
  //      (the same flag the launch path consumes)
  //   4. /dev/dri/card1 (matches the launch-path default)
  // Step 3 is the fix for `--drm-list-modes --drm-device /dev/dri/cardN`,
  // which previously silently fell through to /dev/dri/card1 because the
  // next-positional check at step 2 rejects anything starting with `-`.
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
#if BUILD_BACKEND_DRM_KMS_EGL
    if (list_modes_dev.empty()) {
      list_modes_dev = "/dev/dri/card1";
    }
    return PrintDrmModes(list_modes_dev) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#else  // BUILD_BACKEND_SOFTWARE
    // Empty device → PrintDumbSinkModes scans every /dev/dri/card* so the
    // operator can discover which card to pass to --drm-device.
    return PrintDumbSinkModes(list_modes_dev) == 0 ? EXIT_SUCCESS
                                                   : EXIT_FAILURE;
#endif
  }
#endif

  gLogger = std::make_unique<Logging>();

  const auto configs = Configuration::ParseArgcArgv(argc, argv);
  assert(!configs.empty());

  const App app(configs);

  // Construct the waker (publishing its eventfd) before installing the
  // signal handlers, so HandleShutdownSignal's async-signal-safe wake always
  // has a valid fd to write to.
  (void)MainLoopWaker::instance();
  InstallShutdownHandlers();

  // run the application
  int ret = 0;
  while (running && ret != -1) {
    ret = app.Loop();
  }

  gLogger.reset();

#if BUILD_CRASH_HANDLER
  (void)crash_handler.release();
#endif

  return EXIT_SUCCESS;
}
