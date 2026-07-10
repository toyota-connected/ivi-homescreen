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

#ifndef IHS_SHARED_SRC_LOGGING_LOG_SINK_HPP_
#define IHS_SHARED_SRC_LOGGING_LOG_SINK_HPP_

#include "log_level.hpp"

#include <cstddef>
#include <cstdint>

namespace ihs::dlt {

namespace abi {
struct DltContext;
}

// One drained log record handed to each active sink. Text and tag are
// null-terminated and owned by the ring / context cache; a sink must not retain
// the pointers past the write() call.
struct LogRecord {
  const char* tag;           // context id (ContextEntry::id)
  abi::DltContext* dlt_ctx;  // registered DLT context; null when unregistered
  LogLevel level;
  const char* text;
  std::size_t text_len;
  std::uint64_t ts_ns;  // CLOCK_REALTIME captured once at drain
};

// A log output. The single drain worker owns the sink list and calls write()
// for every record, so implementations need no internal locking. write() must
// be non-blocking-friendly (best-effort I/O); it runs off the producer threads.
class Sink {
 public:
  virtual ~Sink() = default;
  virtual void write(const LogRecord& record) noexcept = 0;
  virtual void flush() noexcept {}
};

}  // namespace ihs::dlt

#endif  // IHS_SHARED_SRC_LOGGING_LOG_SINK_HPP_
