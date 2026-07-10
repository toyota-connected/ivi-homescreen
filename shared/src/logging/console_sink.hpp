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

#ifndef IHS_SHARED_SRC_LOGGING_CONSOLE_SINK_HPP_
#define IHS_SHARED_SRC_LOGGING_CONSOLE_SINK_HPP_

#include "log_sink.hpp"

namespace ihs::dlt {

// Writes "HH:MM:SS.mmm [L] tag: text" lines to stderr, one per record. Adds
// ANSI severity color when stderr is a terminal. The universal fallback sink
// when DLT is unavailable and no file is configured.
class ConsoleSink final : public Sink {
 public:
  ConsoleSink() noexcept;

  void write(const LogRecord& record) noexcept override;
  void flush() noexcept override;

 private:
  bool color_;
};

}  // namespace ihs::dlt

#endif  // IHS_SHARED_SRC_LOGGING_CONSOLE_SINK_HPP_
