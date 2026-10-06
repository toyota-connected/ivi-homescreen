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

// Watchdog::shutdown() returns promptly rather than waiting out the service
// thread's current ping period.
//
// The thread used to sleep in select(0, nullptr, nullptr, nullptr, &timeout) --
// no descriptors, so nothing could wake it. shutdown() cleared running_ and
// joined immediately, which blocked for whatever was left of intervalMs_ / 2:
// 2.5 s on the 5 s default, and up to 60 s under systemd, whose WatchdogSec is
// commonly 120. Reported by a customer as `systemctl stop` and `restart`
// hanging.
//
// One case, because Watchdog is a process-wide singleton whose thread starts in
// its constructor and is joined once. The assertion is a latency bound, which
// is the only thing that distinguishes the fix from the bug.

#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "shell/watchdog/watchdog.h"

namespace {

TEST(WatchdogShutdown, ReturnsWithoutWaitingOutThePingPeriod) {
  Watchdog::init();
  auto& wd = Watchdog::getInstance();

  const uint64_t interval_ms = wd.getTimeoutMs();
  ASSERT_GT(interval_ms, 0U);
  // The thread pings at half the interval, so that is what a stop used to wait.
  const auto half = std::chrono::milliseconds(interval_ms / 2);
  ASSERT_GE(half, std::chrono::milliseconds(500))
      << "interval too short for this bound to mean anything";

  // Let the thread reach its wait, so this measures an interrupted wait rather
  // than a stop that raced the thread's start.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto t0 = std::chrono::steady_clock::now();
  wd.shutdown();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0);

  // Generous against a loaded builder, and still far below the half-interval:
  // the bug's elapsed time was the whole of it.
  EXPECT_LT(elapsed, half / 2)
      << "shutdown took " << elapsed.count()
      << " ms against a half-interval of " << half.count()
      << " ms -- it is waiting out the ping period instead of "
         "being woken";

  // Idempotent: main calls shutdown() and the destructor calls it again.
  const auto t1 = std::chrono::steady_clock::now();
  wd.shutdown();
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t1),
            std::chrono::milliseconds(100))
      << "a second shutdown should be a no-op";
}

}  // namespace
