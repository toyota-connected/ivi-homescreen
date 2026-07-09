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

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>

#include "config/common.h"

// Watchdog source IDs - simple integer constants for maintainability
// - Negative values: reserved for internal embedder use
// - Positive values: reserved for additional/external sources (e.g., plugins,
// tests)
#define WATCHDOG_SOURCE_MAIN_THREAD -1
#define WATCHDOG_SOURCE_RENDER_THREAD -2

typedef int64_t WatchdogSource;

class Watchdog {
 public:
  static Watchdog& getInstance();

  uint64_t getTimeoutMs() const;

  void start(WatchdogSource source);

  void pet(WatchdogSource source);

  void stop(WatchdogSource source);

  void shutdown();

  // Delete copy-constructor and assignment operators
  Watchdog(const Watchdog&) = delete;
  Watchdog& operator=(const Watchdog&) = delete;

 private:
  static constexpr uint64_t kDefaultTimeoutMs = 5'000;  // Default timeout in ms
  uint64_t intervalMs_ = kDefaultTimeoutMs;  // Timeout interval in ms

  std::map<WatchdogSource, std::chrono::time_point<std::chrono::steady_clock>>
      activeSources_;                 // Track timeouts
  std::mutex mutex_;                  // Protect access to activeSources_
  std::atomic<bool> running_{false};  // Whether the watchdog thread is running
  std::thread watchdogThread_;        // Single thread handling timeouts

  // Private constructor for Singleton
  Watchdog();
  ~Watchdog();

  // The main watchdog thread routine
  void watchdogService();
};
