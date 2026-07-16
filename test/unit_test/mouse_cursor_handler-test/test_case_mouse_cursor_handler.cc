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

// Unit tests for MouseCursorHandler.
//
// MouseCursorHandler registers a BinaryMessageHandler on "flutter/mousecursor".
// Tests drive it through the full method-channel round-trip (encode → dispatch
// via captured handler → inspect reply bytes) so HandleMethodCall does not
// need to be public.  The null-view error path and the unknown-method path are
// exercised; the success path (non-null view) is deferred to Phase 3.

#include <memory>
#include <string>

#include <flutter/encodable_value.h>
#include <gtest/gtest.h>

#include "channel_test_utils.h"
#include "stub_binary_messenger.h"

#include "platform/homescreen/mouse_cursor_handler.h"

static constexpr char kChannel[] = "flutter/mousecursor";

// ---- Construction ---------------------------------------------------------

TEST(MouseCursorHandler, ConstructorRegistersChannel) {
  StubBinaryMessenger messenger;
  MouseCursorHandler handler(&messenger, /*view=*/nullptr);
  EXPECT_TRUE(messenger.HasHandler(kChannel));
}

// ---- activateSystemCursor — null view -------------------------------------

TEST(MouseCursorHandler, ActivateSystemCursor_NullView_Error) {
  StubBinaryMessenger messenger;
  MouseCursorHandler handler(&messenger, /*view=*/nullptr);

  // Build an EncodableMap argument as Flutter sends it.
  flutter::EncodableMap args_map;
  args_map[flutter::EncodableValue("device")] =
      flutter::EncodableValue(int32_t{0});
  args_map[flutter::EncodableValue("kind")] =
      flutter::EncodableValue(std::string("basic"));

  const auto reply = DispatchStandard(
      messenger, kChannel, "activateSystemCursor",
      std::make_unique<flutter::EncodableValue>(std::move(args_map)));
  EXPECT_TRUE(EnvelopeIsError(reply));
}

// ---- Unknown method -------------------------------------------------------

TEST(MouseCursorHandler, UnknownMethod_NotImplemented) {
  StubBinaryMessenger messenger;
  MouseCursorHandler handler(&messenger, /*view=*/nullptr);

  const auto reply = DispatchStandard(messenger, kChannel, "noSuchMethod");
  EXPECT_TRUE(EnvelopeIsNotImplemented(reply));
}
