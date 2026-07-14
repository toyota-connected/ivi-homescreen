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

// Fake-priority forwarding test for the libinput -> ihs::log bridge (WS-A1).
// The map is a pure function of the libinput priority enum, so no live
// libinput context is needed: feed each priority, assert the ihs::log level.

#include <libinput.h>

#include "gtest/gtest.h"

#include "backend/software/input/libinput_log_bridge.h"

using homescreen::MapLibinputLogLevel;

TEST(LibinputLogBridge, DebugMapsToDebug) {
  EXPECT_EQ(MapLibinputLogLevel(LIBINPUT_LOG_PRIORITY_DEBUG), IHS_LEVEL_DEBUG);
}

TEST(LibinputLogBridge, InfoMapsToInfo) {
  // The "client bug: event processing lagging" marker is emitted at INFO on
  // current libinput; INFO priority is what SoftwareSeat requests so this line
  // is delivered rather than filtered by the default handler.
  EXPECT_EQ(MapLibinputLogLevel(LIBINPUT_LOG_PRIORITY_INFO), IHS_LEVEL_INFO);
}

TEST(LibinputLogBridge, ErrorMapsToError) {
  EXPECT_EQ(MapLibinputLogLevel(LIBINPUT_LOG_PRIORITY_ERROR), IHS_LEVEL_ERROR);
}

TEST(LibinputLogBridge, UnknownPriorityFallsBackToWarn) {
  // A hypothetical future priority (outside the three libinput defines today)
  // must stay visible and must not be mis-ranked as an error.
  const auto future =
      static_cast<libinput_log_priority>(LIBINPUT_LOG_PRIORITY_ERROR + 10);
  EXPECT_EQ(MapLibinputLogLevel(future), IHS_LEVEL_WARN);
}
