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
#include <map>
#include <string>
#include <vector>

#include "flutter/shell/platform/common/client_wrapper/include/flutter/binary_messenger.h"

// A hand-rolled stub for flutter::BinaryMessenger used in Phase 2 unit tests.
//
// It captures the BinaryMessageHandler registered by a plugin via
// SetMessageHandler() and makes it available through Handler() so the test
// can invoke it directly (simulating an inbound message from Flutter).
//
// Outgoing Send() payloads are recorded per channel in sent_ so tests can
// decode and inspect the plugin's replies.
class StubBinaryMessenger : public flutter::BinaryMessenger {
 public:
  // Send a binary message to a channel.  Records the payload in sent_[channel].
  void Send(const std::string& channel,
            const uint8_t* message,
            size_t message_size,
            flutter::BinaryReply /*reply*/ = nullptr) const override {
    sent_[channel].assign(message, message + message_size);
  }

  // Store the handler the plugin registers for a channel.
  void SetMessageHandler(const std::string& channel,
                         flutter::BinaryMessageHandler handler) override {
    if (handler) {
      handlers_[channel] = std::move(handler);
    } else {
      handlers_.erase(channel);
    }
  }

  // Retrieve the handler registered for |channel|, or nullptr.
  [[nodiscard]] const flutter::BinaryMessageHandler* Handler(
      const std::string& channel) const {
    const auto it = handlers_.find(channel);
    if (it == handlers_.end()) {
      return nullptr;
    }
    return &it->second;
  }

  // True when a handler is registered for |channel|.
  [[nodiscard]] bool HasHandler(const std::string& channel) const {
    return handlers_.count(channel) > 0;
  }

  // The last raw payload sent on |channel|, or empty if nothing was sent.
  [[nodiscard]] const std::vector<uint8_t>& Sent(
      const std::string& channel) const {
    static const std::vector<uint8_t> kEmpty;
    const auto it = sent_.find(channel);
    return it != sent_.end() ? it->second : kEmpty;
  }

  // Clear recorded sends.
  void ClearSent() { sent_.clear(); }

 private:
  std::map<std::string, flutter::BinaryMessageHandler> handlers_;
  // Mutable so Send() (const) can write into it.
  mutable std::map<std::string, std::vector<uint8_t>> sent_;
};
