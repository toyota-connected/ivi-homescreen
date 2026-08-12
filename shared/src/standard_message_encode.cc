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

#include "standard_message_encode.h"

#include <cstring>

namespace ihs::codec {
namespace {

// Wire tags, in the order Flutter's serializer defines them. Written out
// rather than referenced because the enum lives in a header this library does
// not include; the round-trip tests are what keep the two agreeing.
constexpr uint8_t kString = 7;
constexpr uint8_t kFloat64List = 11;
constexpr uint8_t kMap = 13;
constexpr uint8_t kInt32 = 3;

// Sizes below 254 are a single byte; the two sentinels buy 16- and 32-bit
// lengths. A reader that got this wrong would misread everything after it, so
// it matches the serializer exactly rather than always using the wide form.
void WriteSize(std::vector<uint8_t>& out, const size_t size) {
  if (size < 254) {
    out.push_back(static_cast<uint8_t>(size));
  } else if (size <= 0xffff) {
    out.push_back(254);
    const auto value = static_cast<uint16_t>(size);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(value));
  } else {
    out.push_back(255);
    const auto value = static_cast<uint32_t>(size);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(value));
  }
}

// Typed lists are padded so their elements start on a natural boundary,
// measured from the beginning of the message rather than from the list. The
// decoder skips the same number of bytes by the same rule, so getting the
// origin wrong shifts every element.
void WriteAlignment(std::vector<uint8_t>& out, const size_t alignment) {
  const size_t mod = out.size() % alignment;
  if (mod == 0) {
    return;
  }
  out.insert(out.end(), alignment - mod, 0);
}

void WriteInt32(std::vector<uint8_t>& out, const int32_t value) {
  out.push_back(kInt32);
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  out.insert(out.end(), bytes, bytes + sizeof(value));
}

void WriteStringValue(std::vector<uint8_t>& out, const std::string& value) {
  out.push_back(kString);
  WriteSize(out, value.size());
  out.insert(out.end(), value.begin(), value.end());
}

}  // namespace

std::vector<uint8_t> EncodeString(const std::string& value) {
  std::vector<uint8_t> out;
  out.reserve(value.size() + 6);
  WriteStringValue(out, value);
  return out;
}

std::vector<uint8_t> EncodeFloat64List(const std::vector<double>& values) {
  std::vector<uint8_t> out;
  out.reserve(values.size() * sizeof(double) + 16);
  out.push_back(kFloat64List);
  WriteSize(out, values.size());
  if (values.empty()) {
    // An empty list still carries its tag and count; the alignment padding is
    // skipped because there is nothing to align to.
    return out;
  }
  WriteAlignment(out, sizeof(double));
  const auto* bytes = reinterpret_cast<const uint8_t*>(values.data());
  out.insert(out.end(), bytes, bytes + values.size() * sizeof(double));
  return out;
}

std::vector<uint8_t> EncodeStringIntMap(
    const std::vector<std::pair<std::string, int32_t>>& entries) {
  std::vector<uint8_t> out;
  out.push_back(kMap);
  WriteSize(out, entries.size());
  for (const auto& [key, value] : entries) {
    WriteStringValue(out, key);
    WriteInt32(out, value);
  }
  return out;
}

}  // namespace ihs::codec
