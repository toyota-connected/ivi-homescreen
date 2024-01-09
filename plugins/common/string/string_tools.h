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

#ifndef PLUGINS_COMMON_STRING_STRING_TOOLS_H_
#define PLUGINS_COMMON_STRING_STRING_TOOLS_H_

#include <string>
#include <vector>

namespace plugin_common {

class StringTools {
 public:
  /**
   * @brief Split string by token
   * @return std::vector<std::string>
   * @relation
   * internal
   */
  static std::vector<std::string> split(std::string str,
                                        const std::string& token);

  /**
   * @brief trim from end of string (right)
   * @return std::string&
   * @retval String that has specified characters trimmed from right.
   * @relation
   * flutter
   */
  inline static std::string& rtrim(std::string& s, const char* t) {
    s.erase(s.find_last_not_of(t) + 1);
    return s;
  }

  /**
   * @brief trim from beginning of string (left)
   * @return std::string&
   * @retval String that has specified characters trimmed from left.
   * @relation
   * flutter
   */
  inline static std::string& ltrim(std::string& s, const char* t) {
    s.erase(0, s.find_first_not_of(t));
    return s;
  }

  /**
   * @brief trim from both ends of string (right then left)
   * @return std::string&
   * @retval String that has specified characters trimmed from right and left.
   * @relation
   * flutter
   */
  inline static std::string& trim(std::string& s, const char* t) {
    return ltrim(rtrim(s, t), t);
  };
};
}  // namespace plugin_common

#endif  // PLUGINS_COMMON_STRING_STRING_TOOLS_H_