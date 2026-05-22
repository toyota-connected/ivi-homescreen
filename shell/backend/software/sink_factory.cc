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

#include "backend/software/sink_factory.h"

#include <cstdlib>
#include <string>

#include "backend/software/file_sink.h"
#include "backend/software/memory_sink.h"
#include "backend/software/none_sink.h"
#include "logging.h"

std::unique_ptr<ISurfaceSink> MakeSinkFromSpec(const std::string_view spec) {
  if (spec.empty() || spec == "none") {
    spdlog::info("[SoftwareBackend] sink: none (frames discarded)");
    return std::make_unique<NoneSink>();
  }
  if (spec == "memory") {
    spdlog::info("[SoftwareBackend] sink: memory (in-process snapshot)");
    return std::make_unique<MemorySink>();
  }
  // file:<pattern>
  constexpr std::string_view kFilePrefix = "file:";
  if (spec.rfind(kFilePrefix, 0) == 0) {
    std::string pattern(spec.substr(kFilePrefix.size()));
    if (pattern.empty()) {
      spdlog::warn(
          "[SoftwareBackend] sink spec 'file:' has empty pattern; "
          "falling back to NoneSink");
      return std::make_unique<NoneSink>();
    }
    spdlog::info("[SoftwareBackend] sink: file (pattern='{}')", pattern);
    return std::make_unique<FileSink>(std::move(pattern));
  }
  spdlog::warn(
      "[SoftwareBackend] unrecognized sink spec '{}' (valid: none | memory | "
      "file:<pattern>); falling back to NoneSink",
      std::string(spec));
  return std::make_unique<NoneSink>();
}

std::unique_ptr<ISurfaceSink> MakeSinkFromEnv() {
  const char* env = std::getenv("IVI_SW_SINK");
  return MakeSinkFromSpec(env != nullptr ? std::string_view(env)
                                         : std::string_view{});
}
