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

// Unit tests for WatchdogPlugin.
//
// BUILD_WATCHDOG is NOT defined, so all #if BUILD_WATCHDOG code paths are dead
// and Watchdog::getInstance() is never called.
//
// Tests drive WatchdogPlugin through the full method-channel round-trip:
// encode method call → dispatch via captured BinaryMessageHandler → inspect
// reply bytes.  HandleMethodCall and checkWatchdogSource remain private.

#include <cstdint>
#include <memory>
#include <string>

#include <flutter/encodable_value.h>
#include <gtest/gtest.h>

#include "channel_test_utils.h"
#include "stub_binary_messenger.h"

#include "platform/homescreen/watchdog_plugin.h"

static constexpr char kChannel[] = "watchdog";

static std::vector<uint8_t> DispatchWithSource(StubBinaryMessenger& messenger,
                                               const std::string& method,
                                               int32_t source) {
  flutter::EncodableMap args_map;
  args_map[flutter::EncodableValue("source")] = flutter::EncodableValue(source);
  return DispatchStandard(
      messenger, kChannel, method,
      std::make_unique<flutter::EncodableValue>(std::move(args_map)));
}

// ---- Construction ---------------------------------------------------------

TEST(WatchdogPlugin, ConstructorRegistersChannel) {
  StubBinaryMessenger messenger;
  WatchdogPlugin plugin(&messenger);
  EXPECT_TRUE(messenger.HasHandler(kChannel));
}

// ---- get_callbacks --------------------------------------------------------

TEST(WatchdogPlugin, GetCallbacks_ReturnsSuccess) {
  StubBinaryMessenger messenger;
  WatchdogPlugin plugin(&messenger);

  const auto reply = DispatchStandard(messenger, kChannel,
                                      WatchdogPlugin::kMethodGetCallbacks);
  EXPECT_TRUE(EnvelopeIsSuccess(reply));
}

// ---- start (no BUILD_WATCHDOG) --------------------------------------------

TEST(WatchdogPlugin, Start_WithoutWatchdog_Succeeds) {
  StubBinaryMessenger messenger;
  WatchdogPlugin plugin(&messenger);

  const auto reply =
      DispatchWithSource(messenger, WatchdogPlugin::kMethodStart, 0);
  EXPECT_TRUE(EnvelopeIsSuccess(reply));
}

// ---- pet (no BUILD_WATCHDOG) ----------------------------------------------

TEST(WatchdogPlugin, Pet_WithoutWatchdog_Succeeds) {
  StubBinaryMessenger messenger;
  WatchdogPlugin plugin(&messenger);

  const auto reply =
      DispatchWithSource(messenger, WatchdogPlugin::kMethodPet, 0);
  EXPECT_TRUE(EnvelopeIsSuccess(reply));
}

// ---- stop (no BUILD_WATCHDOG) ---------------------------------------------

TEST(WatchdogPlugin, Stop_WithoutWatchdog_Succeeds) {
  StubBinaryMessenger messenger;
  WatchdogPlugin plugin(&messenger);

  const auto reply =
      DispatchWithSource(messenger, WatchdogPlugin::kMethodStop, 0);
  EXPECT_TRUE(EnvelopeIsSuccess(reply));
}

// ---- Unknown method -------------------------------------------------------

TEST(WatchdogPlugin, UnknownMethod_NotImplemented) {
  StubBinaryMessenger messenger;
  WatchdogPlugin plugin(&messenger);

  const auto reply = DispatchStandard(messenger, kChannel, "noSuchMethod");
  EXPECT_TRUE(EnvelopeIsNotImplemented(reply));
}
