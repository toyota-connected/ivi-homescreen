/*
 * Copyright 2024 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "watchdog.h"

#if BUILD_SYSTEMD_WATCHDOG
#include <systemd/sd-daemon.h>
#endif

#include "logging/logging.h"

Watchdog::Watchdog()
    : interval_(std::chrono::microseconds(kDefaultTimeout)), stop_flag_(false) {
#if BUILD_SYSTEMD_WATCHDOG
  uint64_t interval;
  if (sd_watchdog_enabled(0, &interval) > 0) {
    interval_ = std::chrono::microseconds(interval);
    sd_notify(0, "READY=1");
  }
#endif
  spdlog::debug("Watchdog interval set for {} uS", interval_.count());
}

Watchdog::~Watchdog() {
  if (watchdog_thread_.joinable()) {
    stop();
#if BUILD_SYSTEMD_WATCHDOG
    sd_notify(0, "STOPPING=1");
#endif
  }
}

void Watchdog::start() {
  watchdog_thread_ = std::thread([this] {
#if BUILD_SYSTEMD_WATCHDOG
    sd_notifyf(0, "STATUS=Running");
#endif
    while (!stop_flag_) {
      if (std::chrono::steady_clock::now() >= deadline_) {
        spdlog::critical("Watchdog timeout");
        // TODO dump stack
#if BUILD_SYSTEMD_WATCHDOG
        sd_notify(0, "WATCHDOG=trigger");
#else
        exit(EXIT_FAILURE);
#endif
        break;
      }
      std::this_thread::sleep_for(
          std::chrono::milliseconds(1));  // idle until next check
    }
  });
  pet();  // _reset the watchdog deadline to now + interval at the start
}

void Watchdog::stop() {
  stop_flag_ = true;
  if (watchdog_thread_.joinable()) {
    watchdog_thread_.join();
  }
}

void Watchdog::pet() {
  deadline_ = std::chrono::steady_clock::now() + interval_;
#if BUILD_SYSTEMD_WATCHDOG
  sd_notify(0, "WATCHDOG=1");
#endif
}
