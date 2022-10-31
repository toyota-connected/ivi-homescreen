/*
 * Copyright 2020 Toyota Connected North America
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

#include <map>
#include <string>

#include <flutter_embedder.h>

class PlatformChannel {
 protected:
  static PlatformChannel* singleton;
  std::map<std::string, FlutterPlatformMessageCallback>
      m_platform_message_handlers;

  PlatformChannel();

 public:
  PlatformChannel(PlatformChannel& other) = delete;
  PlatformChannel(const PlatformChannel&) = delete;
  const PlatformChannel& operator=(const PlatformChannel&) = delete;

  /**
  * @brief Get instance of PlatformChannel
  * @return PlatformChannel*
  * @retval Pointer to PlatformChannel that is a singleton.
  * @relation
  * flutter
  */
  static PlatformChannel* GetInstance() {
    if (singleton == nullptr)
      singleton = new PlatformChannel;
    return singleton;
  }

  /**
  * @brief Get platform message handler
  * @return std::map<std::string, FlutterPlatformMessageCallback>
  * @retval Handler from channel name to Flutter platform message callback
  * @relation
  * flutter
  */
  std::map<std::string, FlutterPlatformMessageCallback> GetHandler() {
    return m_platform_message_handlers;
  }

  /**
  * @brief Register flutter platform message callback
  * @param[in] channel String of channel name
  * @param[in] callback Flutter platform message callback
  * @return void
  * @relation
  * flutter
  */
  void RegisterCallback(const char* channel,
                        FlutterPlatformMessageCallback callback) {
    m_platform_message_handlers.emplace(std::string(channel), callback);
  }
};
