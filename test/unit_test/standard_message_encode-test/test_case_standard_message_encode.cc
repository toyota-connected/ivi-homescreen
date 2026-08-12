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
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "flutter/standard_message_codec.h"

#include "standard_message_encode.h"

namespace {

// Decodes with Flutter's own codec -- the one the framework decodes with on
// the other side of the embedder call. This is the point of the suite: our
// encoder is a second implementation of someone else's format, and only a
// round trip through theirs can show the two still agree.
flutter::EncodableValue Decode(const std::vector<uint8_t>& bytes) {
  const auto& codec = flutter::StandardMessageCodec::GetInstance();
  auto decoded = codec.DecodeMessage(bytes.data(), bytes.size());
  return decoded == nullptr ? flutter::EncodableValue() : *decoded;
}

}  // namespace

// SemanticsAction.setText carries the replacement text as a plain String.
TEST(StandardMessageEncode, StringRoundTrips) {
  const auto bytes = ihs::codec::EncodeString("Sarah Connor");
  const auto value = Decode(bytes);
  ASSERT_TRUE(std::holds_alternative<std::string>(value)) << "not a string";
  EXPECT_EQ(std::get<std::string>(value), "Sarah Connor");
}

// The length prefix changes shape at 254 and again at 65536. A reader that
// disagreed about which form was used would misread everything after it, so
// each branch is exercised rather than assumed.
TEST(StandardMessageEncode, StringLengthPrefixFormsAllRoundTrip) {
  for (const size_t length : {size_t{0}, size_t{1}, size_t{253}, size_t{254},
                              size_t{255}, size_t{65535}, size_t{65536}}) {
    const std::string original(length, 'x');
    const auto value = Decode(ihs::codec::EncodeString(original));
    ASSERT_TRUE(std::holds_alternative<std::string>(value))
        << "length " << length;
    EXPECT_EQ(std::get<std::string>(value), original) << "length " << length;
  }
}

// Multi-byte UTF-8 must survive: the length is in bytes, not characters, and
// getting that wrong truncates a label mid-sequence.
TEST(StandardMessageEncode, NonAsciiStringRoundTrips) {
  const std::string original = "Fahrertür – 22°C";
  const auto value = Decode(ihs::codec::EncodeString(original));
  ASSERT_TRUE(std::holds_alternative<std::string>(value));
  EXPECT_EQ(std::get<std::string>(value), original);
}

// SemanticsAction.scrollToOffset takes a Float64List of (dx, dy).
TEST(StandardMessageEncode, Float64ListRoundTrips) {
  const std::vector<double> original = {12.5, -400.25};
  const auto value = Decode(ihs::codec::EncodeFloat64List(original));
  ASSERT_TRUE(std::holds_alternative<std::vector<double>>(value))
      << "not a Float64List";
  EXPECT_EQ(std::get<std::vector<double>>(value), original);
}

// Elements are padded to an 8-byte boundary measured from the start of the
// message. The padding is only visible when the preceding bytes leave the
// stream misaligned, which the tag and a short count do -- so this is the
// case that catches an alignment mistake.
TEST(StandardMessageEncode, Float64ListIsAlignedFromTheStartOfTheMessage) {
  const std::vector<double> original = {1.0, 2.0, 3.0};
  const auto bytes = ihs::codec::EncodeFloat64List(original);
  // tag + count is 2 bytes, so 6 bytes of padding must precede the doubles.
  EXPECT_EQ(bytes.size(), 2u + 6u + original.size() * sizeof(double));
  const auto value = Decode(bytes);
  ASSERT_TRUE(std::holds_alternative<std::vector<double>>(value));
  EXPECT_EQ(std::get<std::vector<double>>(value), original);
}

// An empty typed list is tag and count with no alignment padding, because
// Flutter's writer returns before writing any. Asserted as exact bytes rather
// than by round trip, because Flutter's own C++ reader is asymmetric here: it
// calls ReadAlignment unconditionally, so decoding an empty list it just wrote
// reads past the end and logs "Invalid read". Matching the writer is right --
// the wire format is what the Dart side decodes -- so anyone tempted to add
// padding to silence that warning would be breaking the encoding to satisfy a
// reader nothing uses. scrollToOffset always carries two elements, so the case
// is academic in this port regardless.
TEST(StandardMessageEncode, EmptyFloat64ListMatchesTheWriterNotTheReader) {
  const auto bytes = ihs::codec::EncodeFloat64List({});
  ASSERT_EQ(bytes.size(), 2u) << "expected tag and count only";
  EXPECT_EQ(bytes[0], 11);  // kFloat64List
  EXPECT_EQ(bytes[1], 0);   // count
}

// SemanticsAction.setSelection takes a Map<String,int> of base and extent.
TEST(StandardMessageEncode, StringIntMapRoundTrips) {
  const std::vector<std::pair<std::string, int32_t>> original = {
      {"base", 3}, {"extent", 11}};
  const auto value = Decode(ihs::codec::EncodeStringIntMap(original));
  ASSERT_TRUE(std::holds_alternative<flutter::EncodableMap>(value))
      << "not a map";
  const auto& map = std::get<flutter::EncodableMap>(value);
  ASSERT_EQ(map.size(), 2u);
  EXPECT_EQ(std::get<int32_t>(map.at(flutter::EncodableValue("base"))), 3);
  EXPECT_EQ(std::get<int32_t>(map.at(flutter::EncodableValue("extent"))), 11);
}

// A collapsed selection and a zero offset are ordinary values, not absent
// ones: encoding them as anything else would move the caret to the wrong place.
TEST(StandardMessageEncode, ZeroValuesAreCarriedNotDropped) {
  const auto value =
      Decode(ihs::codec::EncodeStringIntMap({{"base", 0}, {"extent", 0}}));
  const auto& map = std::get<flutter::EncodableMap>(value);
  EXPECT_EQ(std::get<int32_t>(map.at(flutter::EncodableValue("base"))), 0);
  EXPECT_EQ(std::get<int32_t>(map.at(flutter::EncodableValue("extent"))), 0);
}

// Negative offsets appear during overscroll, and a text offset is signed.
TEST(StandardMessageEncode, NegativeValuesSurvive) {
  const auto scroll = Decode(ihs::codec::EncodeFloat64List({-1.5, -2.5}));
  EXPECT_EQ(std::get<std::vector<double>>(scroll),
            (std::vector<double>{-1.5, -2.5}));

  const auto selection = Decode(ihs::codec::EncodeStringIntMap({{"base", -1}}));
  const auto& map = std::get<flutter::EncodableMap>(selection);
  EXPECT_EQ(std::get<int32_t>(map.at(flutter::EncodableValue("base"))), -1);
}

TEST(StandardMessageEncode, EmptyMapRoundTrips) {
  const auto value = Decode(ihs::codec::EncodeStringIntMap({}));
  ASSERT_TRUE(std::holds_alternative<flutter::EncodableMap>(value));
  EXPECT_TRUE(std::get<flutter::EncodableMap>(value).empty());
}
