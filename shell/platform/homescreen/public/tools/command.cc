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

#include "command.h"

#include "../logging.h"

namespace plugin_common::Command {

bool Execute(const char* cmd, std::string& result) {
  const auto fp = popen(cmd, "r");
  if (!fp) {
    spdlog::error("[ExecuteCommand] Failed to Execute Command: ({}) {}", errno,
                  strerror(errno));
    spdlog::error("Failed to Execute Command: {}", cmd);
    return false;
  }

  SPDLOG_TRACE("[Command] Execute: {}", cmd);

  auto buf = std::make_unique<char[]>(1024);
  while (fgets(&buf[0], 1024, fp) != nullptr) {
    result.append(&buf[0]);
  }
  buf.reset();

  SPDLOG_TRACE("[Command] Execute Result: [{}] {}", result.size(), result);

  if (const auto status = pclose(fp); status == -1) {
    spdlog::error("[ExecuteCommand] Failed to Close Pipe: ({}) {}", errno,
                  strerror(errno));
    return false;
  }
  return true;
}

}  // namespace plugin_common::Command
