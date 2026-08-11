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

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#include "config/common.h"

#include "app.h"
#include "backend/register_backends.h"
#if BUILD_BACKEND_WAYLAND_LEASED_DRM
#include "backend/wayland_leased_drm/lease_client.h"
#endif
#include "configuration/configuration.h"
#if ENABLE_OSGI
#include "osgi/app_bundle_host.h"
#include "osgi/osgi_config.h"
#include "osgi/startup_orchestrator.h"
#endif
#include "ihs/config.h"
#include "logging/logger.hpp"
#include "logging/logging.h"
#include "main_loop_waker.h"

#if BUILD_CRASH_HANDLER
#include "crash_handler.h"
#endif

#if BUILD_WATCHDOG
#include "watchdog/watchdog.h"
#endif

#include "shutdown_flag.h"

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

// Publish the resolved configuration to ihs_shared so FFI plugins can read it.
// One view scope per parsed Config; global (non-per-view) keys go under
// IHS_CONFIG_GLOBAL. Only set-in-config values are published; a key absent
// here reads as "not present" on the plugin side. Keys are flattened names,
// so extending this set needs no ABI change.
void PublishIhsConfig(const std::vector<Configuration::Config>& configs) {
  IhsConfigBuilder* builder =
      ihs_config_builder_new(static_cast<size_t>(configs.size()));

  if (!configs.empty()) {
    const auto& g = configs.front();
    if (!g.app_id.empty()) {
      ihs_config_builder_set_str(builder, IHS_CONFIG_GLOBAL, "app_id",
                                 g.app_id.c_str());
    }
    if (!g.cursor_theme.empty()) {
      ihs_config_builder_set_str(builder, IHS_CONFIG_GLOBAL, "cursor_theme",
                                 g.cursor_theme.c_str());
    }
    if (g.disable_cursor.has_value()) {
      ihs_config_builder_set_bool(builder, IHS_CONFIG_GLOBAL, "disable_cursor",
                                  *g.disable_cursor ? 1 : 0);
    }
    if (g.debug_backend.has_value()) {
      ihs_config_builder_set_bool(builder, IHS_CONFIG_GLOBAL, "debug_backend",
                                  *g.debug_backend ? 1 : 0);
    }
  }

  for (size_t i = 0; i < configs.size(); ++i) {
    const auto& v = configs[i].view;
    const auto view = static_cast<int32_t>(i);
    if (!v.bundle_path.empty()) {
      ihs_config_builder_set_str(builder, view, "bundle_path",
                                 v.bundle_path.c_str());
    }
    if (!v.window_type.empty()) {
      ihs_config_builder_set_str(builder, view, "window_type",
                                 v.window_type.c_str());
    }
    if (v.width.has_value()) {
      ihs_config_builder_set_int(builder, view, "width", *v.width);
    }
    if (v.height.has_value()) {
      ihs_config_builder_set_int(builder, view, "height", *v.height);
    }
    if (v.pixel_ratio.has_value()) {
      ihs_config_builder_set_double(builder, view, "pixel_ratio",
                                    *v.pixel_ratio);
    }
    if (v.fullscreen.has_value()) {
      ihs_config_builder_set_bool(builder, view, "fullscreen",
                                  *v.fullscreen ? 1 : 0);
    }
    if (v.ivi_surface_id.has_value()) {
      ihs_config_builder_set_int(builder, view, "ivi_surface_id",
                                 *v.ivi_surface_id);
    }
    if (v.accessibility_features.has_value()) {
      ihs_config_builder_set_int(builder, view, "accessibility_features",
                                 *v.accessibility_features);
    }
    if (v.drm_device.has_value()) {
      ihs_config_builder_set_str(builder, view, "drm_device",
                                 v.drm_device->c_str());
    }
  }

  ihs_config_publish(builder);
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
  IHS_LOGGING_START("IHSC", "ivi-homescreen Flutter runtime");

#if BUILD_BACKEND_WAYLAND_LEASED_DRM
  // --lease-list-connectors: ask the compositor what it will actually lease.
  //
  // Handled here, off raw argv and ahead of config parsing, because it must
  // work with no bundle: it is the first thing to run when a leased backend
  // refuses to start, and demanding an app bundle to answer "what do you
  // offer?" would be absurd. (--drm-list-modes sits after parsing and does
  // require one.)
  //
  // This exists because "the compositor implements drm-lease-v1" and "the
  // compositor will lease you a connector" are different facts, and only the
  // second one matters. wlroots offers a connector only when its EDID carries
  // the non-desktop flag, so the usual outcome on a desktop is a device that
  // advertises itself and offers nothing — indistinguishable, without this,
  // from a misconfigured connector name.
  if (const int rc = homescreen::MaybeListLeaseConnectors(argc, argv);
      rc >= 0) {
    return rc;
  }
#endif

  const auto configs = Configuration::ParseArgcArgv(argc, argv);
#if ENABLE_OSGI
  // A pure-OSGi config has no [[view]]: the bundles become the views, and they
  // are added below once App exists. ParseArgcArgv has already rejected an
  // empty list that had no bundles behind it either, so reaching here with none
  // means bundles are coming.
#else
  assert(!configs.empty());
#endif

  // Make the resolved configuration readable by FFI plugins via ihs_shared.
  PublishIhsConfig(configs);

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

#if BUILD_WATCHDOG
  // [watchdog.source_names] is process-level: read it from the --config master
  // file when given, otherwise from the first bundle's config.toml.
  {
    std::string watchdog_config_path;
    if (!configs.empty()) {
      const auto& front = configs.front();
      if (front.config_file && !front.config_file->empty()) {
        watchdog_config_path = *front.config_file;
      } else if (!front.view.bundle_path.empty()) {
        watchdog_config_path =
            front.view.bundle_path + "/" + std::string(kViewConfigToml);
      }
    }
    Watchdog::init(&watchdog_config_path);
  }
#endif

#if INTEGRATION_TEST_SYSTEMD_WATCHDOG
  // Systemd watchdog integration test: register a source and never pet it so
  // the watchdog thread fires, sends WATCHDOG=trigger to NOTIFY_SOCKET, and
  // calls abort(). No Flutter engine or app bundle is needed — this fires
  // before App construction. The script sets NOTIFY_SOCKET to a fake Unix
  // datagram socket and verifies the trigger notification arrives.
  Watchdog::getInstance().start(100);
  ihs::log::info(
      "Systemd watchdog integration test: source 100 registered, awaiting "
      "timeout ({} ms)...",
      Watchdog::getInstance().getTimeoutMs());
  // Sleep well beyond the timeout so the watchdog thread has time to fire.
  std::this_thread::sleep_for(
      std::chrono::milliseconds(Watchdog::getInstance().getTimeoutMs() * 3));
  // Unreachable if the watchdog fired correctly (abort() was called).
  ihs::log::error(
      "Systemd watchdog integration test failed: watchdog did not abort.");
  return 232;
#endif

#if INTEGRATION_TEST_CRASH_HANDLER
  // If we're running the crash handler integration test, trigger a crash and
  // exit immediately. The test runner will then check if the crash was captured
  // successfully.

  ihs::log::info(
      "Running crash handler integration test: triggering crash in 3 "
      "seconds...");
  std::this_thread::sleep_for(std::chrono::seconds(3));

  CrashHandler::trigger_crash();
  ihs::log::error(
      "Crash handler integration test failed: trigger_crash() returned instead "
      "of crashing.");
  return 231;
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
      ihs::log::critical(
          "[main] the '{}' backend does not support --drm-list-modes",
          active.key);
      return EXIT_FAILURE;
    }
    return active.list_modes(list_modes_dev) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  // Scoped so ~App runs before logging is stopped below. Shutdown is where a
  // backend reports what the session did -- the motion-to-photon and vsync
  // summaries, the DRM CRTC restore, the VT keyboard mode being handed back --
  // and all of it is emitted from destructors. Left to run at `return`, after
  // ihs_log_stop(), every one of those lines is written to a logging system
  // that has already gone away, and the operator sees a log that simply stops.
  {
    // Construct the App. It populates the runtime backend registry and resolves
    // the active backend from its configs on first display creation, so it is
    // self-contained (e.g. the app unit test constructs App without main()).
    App app(configs);

    // Construct the waker (publishing its eventfd) before installing the
    // signal handlers, so HandleShutdownSignal's async-signal-safe wake always
    // has a valid fd to write to.
    (void)MainLoopWaker::instance();
    InstallShutdownHandlers();

#if ENABLE_OSGI
    // OSGi bundles are added one at a time rather than being folded into
    // `configs` above, and that is the whole point: App's constructor runs
    // every view's engine back to back, so a bundle in that vector would
    // already be competing for the GPU before anything could wait on it.
    // Critical bundles are therefore brought up here -- after App exists, so
    // there are displays to join, but before Run() -- and each is awaited
    // before the next starts.
    std::unique_ptr<ihs::osgi::BundleStartupOrchestrator> orchestrator;
    ihs::osgi::OsgiConfig osgi_config;
    ihs::osgi::AppBundleHost bundle_host(app, nullptr);
    // Read from argv rather than configs.front(): a pure-OSGi deployment has no
    // views, so there is no config to carry the path.
    std::string osgi_config_path;
    for (int i = 1; i + 1 < argc; ++i) {
      if (std::string_view(argv[i]) == "--config") {
        osgi_config_path = argv[i + 1];
        break;
      }
    }
    if (osgi_config_path.empty() && !configs.empty() &&
        configs.front().config_file) {
      osgi_config_path = *configs.front().config_file;
    }
    if (!osgi_config_path.empty()) {
      if (!ihs::osgi::LoadOsgiConfig(osgi_config_path, osgi_config)) {
        // A malformed [osgi] table is a configuration error the operator has to
        // see, not something to start half of.
        ihs::log::critical("[osgi] configuration rejected; not starting bundles");
        return EXIT_FAILURE;
      }
    }
    if (!osgi_config.empty()) {
      orchestrator = std::make_unique<ihs::osgi::BundleStartupOrchestrator>(
          bundle_host, osgi_config.bundles);
      for (const auto& outcome : orchestrator->StartCriticalPhase()) {
        if (!outcome.ok()) {
          // Reported, not fatal: one broken cluster must not stop the rest of
          // the system from coming up. Which is the right call for a given
          // product is a deployment decision, and it is visible here.
          ihs::log::error("[osgi] critical bundle '{}' failed: {}",
                          outcome.symbolic_name,
                          ihs::osgi::StartupFailureName(outcome.failure));
        }
      }
      // The deferred phases run before Run() rather than from inside the
      // reactor: they do not block on ACTIVE, so the only cost is their
      // staggered launches, and doing it here keeps the startup sequence in
      // one readable place.
      orchestrator->StartDeferredPhases();
    }
#endif

    // Run the application: the shared reactor drives every backend on this
    // (main) thread and blocks until the shutdown signal stops the io_context.
    const int ret = app.Run();
    (void)ret;
  }

  IHS_LOGGING_FLUSH();
  IHS_LOGGING_STOP();

#if BUILD_WATCHDOG
  Watchdog::getInstance().shutdown();
#endif

#if BUILD_CRASH_HANDLER
  (void)crash_handler.release();
#endif

  return EXIT_SUCCESS;
}
