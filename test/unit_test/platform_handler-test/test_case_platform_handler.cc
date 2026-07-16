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

// Unit tests for PlatformHandler.
//
// PlatformHandler registers a BinaryMessageHandler on "flutter/platform".
// Tests drive it through the full method-channel round-trip (encode → dispatch
// via captured handler → inspect reply bytes) so HandleMethodCall does not
// need to be public.

#include <string>

#include <gtest/gtest.h>

#include "channel_test_utils.h"
#include "stub_binary_messenger.h"

#include "platform/homescreen/platform_handler.h"
#include "rapidjson/document.h"

static constexpr char kChannel[] = "flutter/platform";

// Build a JSON method call whose arguments is an array with a single string.
static std::vector<uint8_t> DispatchJsonWithArray(
    StubBinaryMessenger& messenger,
    const std::string& method,
    const std::string& arg0) {
  auto args = std::make_unique<rapidjson::Document>(rapidjson::kArrayType);
  args->PushBack(rapidjson::Value(arg0.c_str(), args->GetAllocator()),
                 args->GetAllocator());
  return DispatchJson(messenger, kChannel, method, std::move(args));
}

// ---- Construction ---------------------------------------------------------

TEST(PlatformHandler, ConstructorRegistersChannel) {
  StubBinaryMessenger messenger;
  PlatformHandler handler(&messenger, nullptr);
  EXPECT_TRUE(messenger.HasHandler(kChannel));
}

// ---- Clipboard.getData — null view ----------------------------------------
// JsonMethodCodec error envelope: ["error_code", "error_message", null]
// kNoWindowError = "Missing window error"

TEST(PlatformHandler, ClipboardGetData_NullView_Error) {
  StubBinaryMessenger messenger;
  PlatformHandler handler(&messenger, /*view=*/nullptr);

  const auto reply =
      DispatchJsonWithArray(messenger, "Clipboard.getData", "text/plain");
  EXPECT_TRUE(JsonEnvelopeIsHandled(reply));
  EXPECT_TRUE(JsonEnvelopeContains(reply, "Missing"));
}

// ---- Clipboard.setData — null view ----------------------------------------

TEST(PlatformHandler, ClipboardSetData_NullView_Error) {
  StubBinaryMessenger messenger;
  PlatformHandler handler(&messenger, /*view=*/nullptr);

  auto args = std::make_unique<rapidjson::Document>(rapidjson::kObjectType);
  args->AddMember("text", "hello", args->GetAllocator());
  const auto reply =
      DispatchJson(messenger, kChannel, "Clipboard.setData", std::move(args));
  EXPECT_TRUE(JsonEnvelopeIsHandled(reply));
  EXPECT_TRUE(JsonEnvelopeContains(reply, "Missing"));
}

// ---- Unknown method -------------------------------------------------------

TEST(PlatformHandler, UnknownMethod_NotImplemented) {
  StubBinaryMessenger messenger;
  PlatformHandler handler(&messenger, /*view=*/nullptr);

  const auto reply = DispatchJson(messenger, kChannel, "NoSuchMethod");
  EXPECT_TRUE(JsonEnvelopeIsNotImplemented(reply));
}
