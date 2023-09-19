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

#include "app.h"
#include "configuration/configuration.h"
#include "logging/logging.h"

#if defined(BUILD_CRASH_HANDLER)
#include "crash_handler.h"
#endif

volatile bool running = true;

std::unique_ptr<Logging> logger_;

/**
 * @brief Signal handler
 * @param[in] signal No use
 * @return void
 * @relation
 * internal
 */
void SignalHandler(int /* signal */) {
  SPDLOG_INFO("Ctl+C");
  running = false;
  exit(0);
}

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
int main(int argc, char** argv) {
  struct Configuration::Config config {
    .app_id{}, .json_configuration_path{}, .cursor_theme{}, .disable_cursor{},
        .disable_cursor_set{}, .wayland_event_mask{}, .debug_backend{},
        .debug_backend_set{}, .view{},
  };

#if defined(BUILD_CRASH_HANDLER)
  auto crash_handler = std::make_unique<CrashHandler>();
#endif

  auto logger_ = std::make_unique<Logging>();

  config = Configuration::ConfigFromArgcArgv(argc, argv);

  auto vm_arg_count = config.view.vm_args.size();
  if (vm_arg_count) {
    SPDLOG_DEBUG("VM Arg Count: {}", vm_arg_count);
    for (auto const& arg : config.view.vm_args) {
      (void)arg;
      SPDLOG_DEBUG(arg);
    }
  }

  auto configs = Configuration::ParseConfig(config);
  for (auto const& c : configs) {
    Configuration::PrintConfig(c);
  }
  assert(!configs.empty());

  App app(configs);

  std::signal(SIGINT, SignalHandler);

  // run the application
  int ret = 0;
  while (running && ret != -1) {
    ret = app.Loop();
  }

  logger_.reset();

#if defined(BUILD_CRASH_HANDLER)
  (void)crash_handler.release();
#endif

  return EXIT_SUCCESS;
}
