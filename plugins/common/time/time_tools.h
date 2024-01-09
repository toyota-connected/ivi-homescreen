/*
 * Copyright 2024 Toyota Connected North America
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

#ifndef PLUGINS_COMMON_TIME_TIME_TOOLS_H_
#define PLUGINS_COMMON_TIME_TIME_TOOLS_H_

#include <chrono>
#include <cstdint>

namespace plugin_common {

class TimeTools {
 public:
  /**
   * @brief Get epoch time in seconds
   * @return current time in epoch seconds
   * @relation
   * internal
   */
  static int64_t GetEpochTimeInSeconds();
};

}  // namespace plugin_common

#endif  // PLUGINS_COMMON_TIME_TIME_TOOLS_H_