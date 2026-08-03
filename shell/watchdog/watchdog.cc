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

#include <filesystem>
#include <iostream>

#if BUILD_SYSTEMD_WATCHDOG
#include <systemd/sd-daemon.h>
#include <chrono>
#include <thread>
#endif

#define TOML_EXCEPTIONS 0
#include <tomlplusplus/toml.hpp>

#include "logging/logging.h"
#include "stats.h"

Watchdog* instance = nullptr;

Watchdog& Watchdog::getInstance() {
  if (!instance) {
    throw std::runtime_error("Watchdog::getInstance() called before init()");
  }

  return *instance;
}

uint64_t Watchdog::getTimeoutMs() const {
  return intervalMs_;
}

void Watchdog::init(const std::string* config_path) {
  if (!instance) {
    instance = new Watchdog(config_path);
  } else {
    ihs::log::warn("Watchdog: init() already called; ignoring");
  }
}

void Watchdog::setSourceName(const WatchdogSource source,
                             const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  sourceNames_[source] = name;
  ihs::log::debug("Watchdog: source {} named '{}'",
                  static_cast<int64_t>(source), name);
}

void Watchdog::loadConfig(const std::string& config_path) {
  if (config_path.empty()) {
    return;
  }

  const std::filesystem::path toml_path(config_path);
  if (!std::filesystem::exists(toml_path)) {
    ihs::log::warn("Watchdog: config file not found at {}", toml_path.string());
    return;
  }

  auto result = toml::parse_file(toml_path.string());
  if (!result) {
    ihs::log::error("Watchdog: TOML parsing failed: {} — {}",
                    toml_path.string(), result.error().description());
    return;
  }

  auto& tbl = result.table();
  const auto* watchdog_section = tbl.get("watchdog");
  if (!watchdog_section || !watchdog_section->is_table()) {
    return;
  }
  const auto* wdog_tbl = watchdog_section->as_table();

  // [watchdog] timeout_ms — skipped when systemd owns the interval.
  if (!systemdIntervalActive_) {
    if (auto ms = wdog_tbl->get("timeout_ms")) {
      if (auto val = ms->value<uint64_t>(); val && *val > 0) {
        intervalMs_ = *val;
        ihs::log::debug("Watchdog: timeout set to {} ms from config",
                        intervalMs_);
      } else {
        ihs::log::warn(
            "Watchdog: invalid timeout_ms in config, using default ({} ms)",
            intervalMs_);
      }
    }
  }

  // [watchdog.source_names] — keys are source IDs (decimal strings).
  const auto* names_node = wdog_tbl->get("source_names");
  if (!names_node || !names_node->is_table()) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [key, val] : *names_node->as_table()) {
    if (auto name = val.value<std::string>()) {
      try {
        const WatchdogSource source =
            static_cast<WatchdogSource>(std::stoll(std::string(key)));
        sourceNames_[source] = *name;
        ihs::log::debug("Watchdog: config source {} = '{}'",
                        static_cast<int64_t>(source), *name);
      } catch (const std::exception&) {
        ihs::log::warn("Watchdog: ignoring non-integer source_names key '{}'",
                       std::string(key));
      }
    }
  }
}

Watchdog::Watchdog(const std::string* config_path) {
  if (config_path) {
    loadConfig(*config_path);
  }

#if BUILD_SYSTEMD_WATCHDOG
  uint64_t interval;
  if (sd_watchdog_enabled(0, &interval) > 0) {
    intervalMs_ = interval / 1000;
    systemdIntervalActive_ = true;
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
  const auto nameIt = sourceNames_.find(source);
  ihs::log::trace("Watchdog started for source {} ({})",
                  static_cast<int64_t>(source),
                  nameIt != sourceNames_.end() ? nameIt->second : "?");
}

void Watchdog::pet(const WatchdogSource source) {
  std::lock_guard<std::mutex> lock(mutex_);

  const auto nameIt = sourceNames_.find(source);
  const auto& sourceName = nameIt != sourceNames_.end() ? nameIt->second : "?";
  ihs::log::trace("Watchdog pet received for source {} ({})",
                  static_cast<int64_t>(source), sourceName);

  if (const auto it = activeSources_.find(source); it == activeSources_.end()) {
    ihs::log::warn("Source {} ({}) is not being monitored.",
                   static_cast<int64_t>(source), sourceName);
    return;
  }

  // Reset the timer of the source to the current time
  activeSources_[source] = std::chrono::steady_clock::now();
}

void Watchdog::stop(const WatchdogSource source) {
  std::lock_guard<std::mutex> lock(mutex_);

  const auto nameIt = sourceNames_.find(source);
  const auto& sourceName = nameIt != sourceNames_.end() ? nameIt->second : "?";
  if (activeSources_.find(source) == activeSources_.end()) {
    ihs::log::warn("Source {} ({}) is not being monitored.",
                   static_cast<int64_t>(source), sourceName);
  }

  // Remove the source from active sources
  activeSources_.erase(source);
  ihs::log::trace("Watchdog stopped for source {} ({})",
                  static_cast<int64_t>(source), sourceName);
}

void Watchdog::shutdown() {
  running_.store(false);
#if BUILD_SYSTEMD_WATCHDOG
  sd_notify(0, "STOPPING=1");
#endif

  if (watchdogThread_.joinable()) {
    watchdogThread_.join();
  }
  ihs::log::debug("Watchdog service shut down.");
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
      ihs::log::debug("Threads: {}, VIRT: {}, RES: {}", stats.num_threads,
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
          const auto nameIt2 = sourceNames_.find(source);
          ihs::log::error(
              "Watchdog Timeout: source {} ({})", static_cast<int64_t>(source),
              nameIt2 != sourceNames_.end() ? nameIt2->second : "?");
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
      ihs::log::error("Error in select call; watchdog thread exiting.");
    }
  }

  ihs::log::debug("Watchdog service exiting.");
}
