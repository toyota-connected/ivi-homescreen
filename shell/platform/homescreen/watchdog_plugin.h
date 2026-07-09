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

#include <binary_messenger.h>
#include <method_channel.h>
#include <method_result.h>

#include "config/common.h"

#if BUILD_WATCHDOG
#include "shell/watchdog.h"
#endif

class WatchdogPlugin {
 public:
  static constexpr char kChannelName[] = "watchdog";
  static constexpr char kMethodGetCallbacks[] = "get_callbacks";
  static constexpr char kMethodStart[] = "start";
  static constexpr char kMethodPet[] = "pet";
  static constexpr char kMethodStop[] = "stop";

  explicit WatchdogPlugin(flutter::BinaryMessenger* messenger);

 private:
  static void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  /**
   * @brief Validates if the given source is non-negative.
   * @param[in] source The integer identifier of the watchdog source.
   * @return True if the source is valid (>= 0); false otherwise.
   */
  static bool checkWatchdogSource(int64_t source);

  /**
   * @brief Starts watchdog timer for a given source.
   */
  static void externalStart(int64_t source);

  /**
   * @brief Pets watchdog timer for a given source.
   */
  static void externalPet(int64_t source);

  /**
   * @brief Stops watchdog timer for a given source.
   */
  static void externalStop(int64_t source);

  std::unique_ptr<flutter::MethodChannel<>> channel_;
};
