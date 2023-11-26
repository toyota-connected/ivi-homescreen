/*
 * Copyright 2023 Toyota Connected North America
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

#include <flutter/standard_method_codec.h>
#include <shell/platform/embedder/embedder.h>

#include <logging.h>
#include <mutex>

class FlutterView;

class LoggingPlugin {
 public:
  static constexpr char kChannelName[] = "logging";
  static constexpr char kMethodGetLoggingCallbackFptr[] =
      "get_logging_callback_fptr";

  typedef void (*LoggerFunction)(int level,
                                 const char* context,
                                 const char* message);

  /**
   * @brief Handle a platform message from the Flutter engine.
   * @param[in] message The message from the Flutter engine.
   * @param[in] userdata The user data.
   * @return void
   * @relation
   * flutter
   */
  static void OnPlatformMessage(const FlutterPlatformMessage* message,
                                void* userdata);

 private:
  /**
   * @brief Callback that writes to log.
   * @param[in] level Severity Value.
   * @param[in] context DLT Context ID.
   * @param[in] message Message to be logged.
   * @return void
   * @relation
   * flutter
   */
  static void OnLogMessage(int level,
                               const char* context,
                               const char* message);
};
