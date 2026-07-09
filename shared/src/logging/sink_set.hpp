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

#ifndef IHS_SHARED_SRC_LOGGING_SINK_SET_HPP_
#define IHS_SHARED_SRC_LOGGING_SINK_SET_HPP_

#include "log_level.hpp"
#include "log_sink.hpp"

#include <memory>
#include <vector>

namespace ihs::dlt {

class LibDltLoader;

// The resolved logging output configuration, evaluated once at ihs_log_start
// from the environment (IHS_LOG_SINK, IHS_LOG_LEVEL, IHS_LOG_FILE*).
struct SinkSet {
  std::vector<std::unique_ptr<Sink>> sinks;
  LogLevel level_floor = LogLevel::Verbose;  // records below this never enqueue
};

// Build the sink list from the environment. IHS_LOG_SINK is a comma list of
// dlt|console|file (default "dlt"); an unavailable sink warns once and falls
// back to console; the console sink is always ensured so a well-formed program
// never logs silently to nowhere.
SinkSet build_sink_set_from_env(LibDltLoader& loader);

}  // namespace ihs::dlt

#endif  // IHS_SHARED_SRC_LOGGING_SINK_SET_HPP_
