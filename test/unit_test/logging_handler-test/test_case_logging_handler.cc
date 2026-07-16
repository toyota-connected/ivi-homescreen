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

// Unit tests for LoggingHandler.
//
// LoggingHandler registers a BinaryMessageHandler on "logging" during
// construction.  Tests drive it through the full method-channel round-trip:
// encode method call → dispatch through captured handler → inspect reply bytes.

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "channel_test_utils.h"
#include "stub_binary_messenger.h"

#include "platform/homescreen/logging_handler.h"

static constexpr char kChannel[] = "logging";

// ---- Construction ---------------------------------------------------------

TEST(LoggingHandler, ConstructorRegistersChannel) {
  StubBinaryMessenger messenger;
  LoggingHandler handler(&messenger, nullptr);
  EXPECT_TRUE(messenger.HasHandler(kChannel));
}

// ---- get_logging_callback_fptr -------------------------------------------

TEST(LoggingHandler, GetCallbackFptr_ReturnsSuccess) {
  StubBinaryMessenger messenger;
  LoggingHandler handler(&messenger, nullptr);

  const auto reply =
      DispatchStandard(messenger, kChannel, "get_logging_callback_fptr");
  EXPECT_TRUE(EnvelopeIsSuccess(reply));
}

// ---- Unknown method -------------------------------------------------------

TEST(LoggingHandler, UnknownMethod_NotImplemented) {
  StubBinaryMessenger messenger;
  LoggingHandler handler(&messenger, nullptr);

  const auto reply = DispatchStandard(messenger, kChannel, "no_such_method");
  EXPECT_TRUE(EnvelopeIsNotImplemented(reply));
}
