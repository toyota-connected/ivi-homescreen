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

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "flutter/standard_message_codec.h"

#include "ihs/ihs_semantics.h"

#include "accessibility/semantics_action_args.h"

using accessibility::EncodeActionArguments;

namespace {

flutter::EncodableValue Decode(const std::vector<uint8_t>& bytes) {
  const auto& codec = flutter::StandardMessageCodec::GetInstance();
  auto decoded = codec.DecodeMessage(bytes.data(), bytes.size());
  return decoded == nullptr ? flutter::EncodableValue() : *decoded;
}

std::vector<uint8_t> Bytes(const void* data, const size_t length) {
  const auto* p = static_cast<const uint8_t*>(data);
  return {p, p + length};
}

}  // namespace

// SetText carries the replacement string. The framework reads it as a String,
// so anything else silently replaces nothing.
TEST(SemanticsActionArgs, SetTextEncodesAString) {
  const std::string text = "Sarah Connor";
  const auto encoded = EncodeActionArguments(
      IHS_SEMANTICS_ACTION_SET_TEXT,
      reinterpret_cast<const uint8_t*>(text.data()), text.size());
  ASSERT_TRUE(encoded.has_value());
  const auto value = Decode(*encoded);
  ASSERT_TRUE(std::holds_alternative<std::string>(value));
  EXPECT_EQ(std::get<std::string>(value), text);
}

// The length is the size, not a terminator, so text containing a NUL survives
// rather than being truncated at it.
TEST(SemanticsActionArgs, SetTextKeepsEmbeddedNuls) {
  const std::string text("a\0b", 3);
  const auto encoded = EncodeActionArguments(
      IHS_SEMANTICS_ACTION_SET_TEXT,
      reinterpret_cast<const uint8_t*>(text.data()), text.size());
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(std::get<std::string>(Decode(*encoded)).size(), 3u);
}

TEST(SemanticsActionArgs, SetTextNonAsciiSurvives) {
  const std::string text = "Fahrertür – 22°C";
  const auto encoded = EncodeActionArguments(
      IHS_SEMANTICS_ACTION_SET_TEXT,
      reinterpret_cast<const uint8_t*>(text.data()), text.size());
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(std::get<std::string>(Decode(*encoded)), text);
}

// The framework reads the selection by key, so the names are contract.
TEST(SemanticsActionArgs, SetSelectionEncodesBaseAndExtentByName) {
  const int32_t selection[2] = {3, 11};
  const auto raw = Bytes(selection, sizeof(selection));
  const auto encoded = EncodeActionArguments(IHS_SEMANTICS_ACTION_SET_SELECTION,
                                             raw.data(), raw.size());
  ASSERT_TRUE(encoded.has_value());

  const auto value = Decode(*encoded);
  ASSERT_TRUE(std::holds_alternative<flutter::EncodableMap>(value));
  const auto& map = std::get<flutter::EncodableMap>(value);
  EXPECT_EQ(std::get<int32_t>(map.at(flutter::EncodableValue("base"))), 3);
  EXPECT_EQ(std::get<int32_t>(map.at(flutter::EncodableValue("extent"))), 11);
}

// A collapsed selection at the start of a field is zeros, which is an ordinary
// value and must not be mistaken for an absent argument.
TEST(SemanticsActionArgs, SetSelectionCarriesZeroes) {
  const int32_t selection[2] = {0, 0};
  const auto raw = Bytes(selection, sizeof(selection));
  const auto encoded = EncodeActionArguments(IHS_SEMANTICS_ACTION_SET_SELECTION,
                                             raw.data(), raw.size());
  ASSERT_TRUE(encoded.has_value());
  // Bound to a named value first: std::get on the temporary Decode returns
  // hands back a reference into it, and binding that to a const& does not
  // extend the temporary's life.
  const auto value = Decode(*encoded);
  const auto& map = std::get<flutter::EncodableMap>(value);
  EXPECT_EQ(std::get<int32_t>(map.at(flutter::EncodableValue("base"))), 0);
  EXPECT_EQ(std::get<int32_t>(map.at(flutter::EncodableValue("extent"))), 0);
}

// ScrollToOffset takes a Float64List of (dx, dy).
TEST(SemanticsActionArgs, ScrollToOffsetEncodesAFloat64List) {
  const double offset[2] = {12.5, -400.25};
  const auto raw = Bytes(offset, sizeof(offset));
  const auto encoded = EncodeActionArguments(
      IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET, raw.data(), raw.size());
  ASSERT_TRUE(encoded.has_value());

  const auto value = Decode(*encoded);
  ASSERT_TRUE(std::holds_alternative<std::vector<double>>(value));
  EXPECT_EQ(std::get<std::vector<double>>(value),
            (std::vector<double>{12.5, -400.25}));
}

// A truncated argument is refused rather than padded. The framework drops a
// malformed payload in silence, so forwarding one would show the caller a
// dispatch that succeeded and did nothing.
TEST(SemanticsActionArgs, ShortArgumentsAreRefused) {
  const uint8_t stub[4] = {0, 0, 0, 0};
  EXPECT_FALSE(EncodeActionArguments(IHS_SEMANTICS_ACTION_SET_SELECTION, stub,
                                     sizeof(stub))
                   .has_value());
  EXPECT_FALSE(EncodeActionArguments(IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET,
                                     stub, sizeof(stub))
                   .has_value());
}

TEST(SemanticsActionArgs, MissingArgumentsAreRefused) {
  for (const uint64_t action :
       {IHS_SEMANTICS_ACTION_SET_TEXT, IHS_SEMANTICS_ACTION_SET_SELECTION,
        IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET}) {
    EXPECT_FALSE(EncodeActionArguments(action, nullptr, 0).has_value())
        << "action 0x" << std::hex << action;
  }
}

// An action that takes nothing encodes to nothing, so a caller can encode
// unconditionally rather than branching on which action it holds.
TEST(SemanticsActionArgs, ArgumentlessActionsEncodeEmpty) {
  const auto encoded =
      EncodeActionArguments(IHS_SEMANTICS_ACTION_TAP, nullptr, 0);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_TRUE(encoded->empty());
}

// Data handed to an action that ignores it is a caller error worth reporting:
// the action would appear to succeed while the argument went nowhere.
TEST(SemanticsActionArgs, DataOnAnArgumentlessActionIsRefused) {
  const uint8_t stray[2] = {1, 2};
  EXPECT_FALSE(
      EncodeActionArguments(IHS_SEMANTICS_ACTION_TAP, stray, sizeof(stray))
          .has_value());
}
