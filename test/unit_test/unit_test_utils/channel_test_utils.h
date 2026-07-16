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

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <flutter/encodable_value.h>
#include <flutter/method_codec.h>
#include <flutter/standard_method_codec.h>
#include "flutter/shell/platform/common/json_method_codec.h"

#include "stub_binary_messenger.h"

// ---- Envelope byte inspection ----------------------------------------
//
// flutter::StandardMethodCodec and flutter::JsonMethodCodec both use:
//   0x00 ... → Success
//   0x01 ... → Error
//   empty    → NotImplemented
//
// For JsonMethodCodec the JSON text follows the tag byte.

// ---- StandardMethodCodec envelope helpers --------------------------------
// Tag byte: 0x00 = Success, 0x01 = Error, empty = NotImplemented.

inline bool EnvelopeIsNotImplemented(const std::vector<uint8_t>& reply) {
  return reply.empty();
}
inline bool EnvelopeIsSuccess(const std::vector<uint8_t>& reply) {
  return !reply.empty() && reply[0] == 0x00;
}
inline bool EnvelopeIsError(const std::vector<uint8_t>& reply) {
  return !reply.empty() && reply[0] == 0x01;
}

// ---- JsonMethodCodec envelope helpers ------------------------------------
// Success: JSON array with 1 element: [value]
// Error:   JSON array with 3 elements: [code, message, details]
// Both start with '['; NotImplemented is empty bytes.

inline bool JsonEnvelopeIsNotImplemented(const std::vector<uint8_t>& reply) {
  return reply.empty();
}
// Search for substring in the JSON bytes (safe: JSON bytes are plain ASCII).
inline bool JsonEnvelopeContains(const std::vector<uint8_t>& reply,
                                 const char* substring) {
  if (reply.empty() || substring == nullptr) {
    return false;
  }
  const std::string s(reply.begin(), reply.end());
  return s.find(substring) != std::string::npos;
}
// An error response from JsonMethodCodec has 3 array elements: code, msg,
// details.  Success has 1.  A quick heuristic: error replies contain a
// comma-separated pair after the code string, so at least two commas exist.
// Callers may prefer JsonEnvelopeContains with a known error-code substring.
inline bool JsonEnvelopeIsHandled(const std::vector<uint8_t>& reply) {
  return !reply.empty();  // any non-empty JSON reply means it was handled
}

// ---- StandardMethodCodec dispatcher -----------------------------------

inline std::vector<uint8_t> DispatchStandard(
    StubBinaryMessenger& messenger,
    const std::string& channel,
    const std::string& method,
    std::unique_ptr<flutter::EncodableValue> args = nullptr) {
  flutter::MethodCall<flutter::EncodableValue> call(method, std::move(args));
  const auto& codec = flutter::StandardMethodCodec::GetInstance();
  const auto encoded = codec.EncodeMethodCall(call);

  const auto* handler = messenger.Handler(channel);
  if (!handler) {
    return {};
  }

  std::vector<uint8_t> reply;
  (*handler)(encoded->data(), encoded->size(),
             [&reply](const uint8_t* r, size_t s) {
               if (r != nullptr) {
                 reply.assign(r, r + s);
               }
             });
  return reply;
}

// ---- JsonMethodCodec dispatcher ---------------------------------------

inline std::vector<uint8_t> DispatchJson(
    StubBinaryMessenger& messenger,
    const std::string& channel,
    const std::string& method,
    std::unique_ptr<rapidjson::Document> args = nullptr) {
  flutter::MethodCall<rapidjson::Document> call(method, std::move(args));
  const auto& codec = flutter::JsonMethodCodec::GetInstance();
  const auto encoded = codec.EncodeMethodCall(call);

  const auto* handler = messenger.Handler(channel);
  if (!handler) {
    return {};
  }

  std::vector<uint8_t> reply;
  (*handler)(encoded->data(), encoded->size(),
             [&reply](const uint8_t* r, size_t s) {
               if (r != nullptr) {
                 reply.assign(r, r + s);
               }
             });
  return reply;
}
