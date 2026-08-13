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

#include "semantics_action_args.h"

#include <cstring>
#include <string>
#include <utility>

#include "flutter/standard_message_codec.h"

#include "ihs/ihs_semantics.h"

namespace accessibility {
namespace {

// The hub's plain layouts, mirrored from ihs_semantics.h. Sizes are fixed so a
// short buffer is a caller error rather than something to pad or guess at.
constexpr size_t kSelectionBytes = sizeof(int32_t) * 2;
constexpr size_t kOffsetBytes = sizeof(double) * 2;

std::vector<uint8_t> Encode(const flutter::EncodableValue& value) {
  const auto& codec = flutter::StandardMessageCodec::GetInstance();
  auto encoded = codec.EncodeMessage(value);
  // Moved rather than copied: the codec hands back an owning pointer, and the
  // buffer is ours to take.
  return encoded != nullptr ? std::move(*encoded) : std::vector<uint8_t>{};
}

}  // namespace

std::optional<std::vector<uint8_t>> EncodeActionArguments(
    const uint64_t action,
    const uint8_t* data,
    const size_t data_length) {
  const bool empty = data == nullptr || data_length == 0;

  if (action == IHS_SEMANTICS_ACTION_SET_TEXT) {
    if (empty) {
      return std::nullopt;  // replacing text with nothing still needs a string
    }
    // The bytes are the text itself, UTF-8, with the length carrying the size
    // -- no terminator, since a caller may legitimately send one containing a
    // NUL and truncating there would silently shorten it.
    return Encode(flutter::EncodableValue(
        std::string(reinterpret_cast<const char*>(data), data_length)));
  }

  if (action == IHS_SEMANTICS_ACTION_SET_SELECTION) {
    if (empty || data_length != kSelectionBytes) {
      return std::nullopt;
    }
    int32_t base = 0;
    int32_t extent = 0;
    std::memcpy(&base, data, sizeof(base));
    std::memcpy(&extent, data + sizeof(base), sizeof(extent));
    // The framework reads a Map rather than a pair, and reads it by name, so
    // the keys are part of the contract rather than decoration.
    flutter::EncodableMap selection;
    selection[flutter::EncodableValue("base")] = flutter::EncodableValue(base);
    selection[flutter::EncodableValue("extent")] =
        flutter::EncodableValue(extent);
    return Encode(flutter::EncodableValue(selection));
  }

  if (action == IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET) {
    if (empty || data_length != kOffsetBytes) {
      return std::nullopt;
    }
    double offset[2] = {0.0, 0.0};
    std::memcpy(offset, data, sizeof(offset));
    return Encode(
        flutter::EncodableValue(std::vector<double>{offset[0], offset[1]}));
  }

  if (action == IHS_SEMANTICS_ACTION_CUSTOM_ACTION) {
    if (empty || data_length != sizeof(int32_t)) {
      return std::nullopt;
    }
    int32_t custom_action_id = 0;
    std::memcpy(&custom_action_id, data, sizeof(custom_action_id));
    // A bare int, not a map: the framework looks the handler up by this id
    // directly, so wrapping it would not resolve to anything.
    return Encode(flutter::EncodableValue(custom_action_id));
  }

  // Every other action takes no argument. Data supplied for one of those is
  // refused rather than dropped: a caller that encoded something expects it to
  // arrive, and an action that ignores it would look like it had worked.
  if (!empty) {
    return std::nullopt;
  }
  return std::vector<uint8_t>{};
}

}  // namespace accessibility
