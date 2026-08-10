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

#include "priority.h"

namespace ihs::osgi {

bool ParsePriority(const std::string_view text, Priority& out) {
  if (text == "critical") {
    out = Priority::kCritical;
    return true;
  }
  if (text == "normal") {
    out = Priority::kNormal;
    return true;
  }
  if (text == "background") {
    out = Priority::kBackground;
    return true;
  }
  return false;
}

std::string_view PriorityName(const Priority priority) {
  switch (priority) {
    case Priority::kCritical:
      return "critical";
    case Priority::kNormal:
      return "normal";
    case Priority::kBackground:
      return "background";
  }
  return "normal";
}

}  // namespace ihs::osgi
