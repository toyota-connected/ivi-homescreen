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

#include "string_tools.h"

namespace plugin_common {

std::vector<std::string> StringTools::split(std::string str,
                                            const std::string& token) {
  std::vector<std::string> result;
  while (!str.empty()) {
    const auto index = str.find(token);
    if (index != std::string::npos) {
      result.push_back(str.substr(0, index));
      str = str.substr(index + token.size());
      if (str.empty())
        result.push_back(str);
    } else {
      result.push_back(str);
      str.clear();
    }
  }
  return result;
}

}  // namespace plugin_common