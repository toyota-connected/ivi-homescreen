/*
 * Copyright 2026 Toyota Connected North America
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

#include <iostream>

#if BUILD_SYSTEMD_WATCHDOG
#include <systemd/sd-daemon.h>
#include <chrono>
#include <thread>
#endif

#include "logging/logging.h"
#include "stats.h"

Watchdog& Watchdog::getInstance() {
  static Watchdog instance;
  return instance;
}

uint64_t Watchdog::getTimeoutMs() const {
  return intervalMs_;
}

Watchdog::Watchdog() {
#if BUILD_SYSTEMD_WATCHDOG
  uint64_t interval;
  if (sd_watchdog_enabled(0, &interval) > 0) {
    intervalMs_ = interval / 1000;
    sd_notify(0, "READY=1");
  }
  sd_notifyf(0, "STATUS=Running");
#endif
  running_.store(true);
  watchdogThread_ = std::thread(&Watchdog::watchdogService, this);
}

Watchdog::~Watchdog() {
  shutdown();
}

void Watchdog::start(const WatchdogSource source) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Record the start time for the source
  activeSources_[source] = std::chrono::steady_clock::now();
  spdlog::debug("Watchdog started for source {}", static_cast<int64_t>(source));
}

void Watchdog::pet(const WatchdogSource source) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (const auto it = activeSources_.find(source); it == activeSources_.end()) {
    spdlog::warn("Source {} is not being monitored.",
                 static_cast<int64_t>(source));
    return;
  }

  // Reset the timer of the source to the current time
  activeSources_[source] = std::chrono::steady_clock::now();
}

void Watchdog::stop(const WatchdogSource source) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (activeSources_.find(source) == activeSources_.end()) {
    spdlog::warn("Source {} is not being monitored.",
                 static_cast<int64_t>(source));
  }

  // Remove the source from active sources
  activeSources_.erase(source);
  spdlog::debug("Watchdog stopped for source {}", static_cast<int64_t>(source));
}

void Watchdog::shutdown() {
  running_.store(false);
#if BUILD_SYSTEMD_WATCHDOG
  sd_notify(0, "STOPPING=1");
#endif

  if (watchdogThread_.joinable()) {
    watchdogThread_.join();
  }
  spdlog::debug("Watchdog service shut down.");
}

void Watchdog::watchdogService() {
  while (running_.load()) {
    fd_set readfds;
    FD_ZERO(&readfds);

    timeval timeout{};

    // Convert 1/2 of the interval to seconds and microseconds
    auto halfIntervalMs = intervalMs_ / 2;
    timeout.tv_sec = static_cast<__time_t>(halfIntervalMs) / 1000;
    timeout.tv_usec = static_cast<__suseconds_t>(halfIntervalMs % 1000) * 1000;

    // Wait for the timeout using select
    if (int ret = select(0, &readfds, nullptr, nullptr, &timeout);
        ret == 0) {  // Timeout occurred
      std::lock_guard<std::mutex> lock(mutex_);

#if BUILD_SYSTEMD_WATCHDOG
      sd_notify(0, "WATCHDOG=1");
#endif

      Stats::ProcessStats stats{};
      Stats::getSelfStats(stats);
      spdlog::debug("Threads: {}, VIRT: {}, RES: {}", stats.num_threads,
                    stats.virtual_memory, stats.resident_set_size);

      // Check for any timeouts
      auto now = std::chrono::steady_clock::now();
      for (auto it = activeSources_.begin(); it != activeSources_.end();) {
        auto source = it->first;

        if (auto startTime = it->second;
            std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                  startTime)
                .count() >= static_cast<int64_t>(intervalMs_)) {
          // Timeout occurred for this source
        ihs::log::critical("Watchdog timeout");
#if BUILD_SYSTEMD_WATCHDOG
          sd_notify(0, "WATCHDOG=trigger");
#endif
          running_.store(false);
          abort();

          // After timeout behavior: remove the source
          it = activeSources_.erase(it);
        } else {
          ++it;
        }
      }
    } else if (ret < 0) {
      // Handles errors in the select call
      spdlog::error("Error in select call; watchdog thread exiting.");
    }
  }

  spdlog::debug("Watchdog service exiting.");
}
