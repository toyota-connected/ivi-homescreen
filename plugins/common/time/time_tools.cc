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

#include "time_tools.h"

namespace plugin_common {

int64_t TimeTools::GetEpochTimeInSeconds() {
  const auto now = std::chrono::system_clock::now();
  const auto epoch = now.time_since_epoch();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(epoch);
  return seconds.count();
}
}  // namespace plugin_common