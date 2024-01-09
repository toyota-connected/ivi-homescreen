/*
 * Copyright 2023-2024 Toyota Connected North America
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

#ifndef PLUGINS_COMMON_TOOLS_COMMAND_H_
#define PLUGINS_COMMON_TOOLS_COMMAND_H_

#include <linux/limits.h>

namespace plugin_common {

class Command {
 public:
  /**
   * @brief Execute Command and return result
   * @return bool
   * @relation
   * internal
   */
  static bool Execute(const char* cmd, char (&result)[PATH_MAX]);
};

}  // namespace plugin_common

#endif  // PLUGINS_COMMON_TOOLS_COMMAND_H_