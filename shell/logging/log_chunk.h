/*
 * Copyright 2020-2026 Toyota Connected North America
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

#ifndef SHELL_LOGGING_LOG_CHUNK_H_
#define SHELL_LOGGING_LOG_CHUNK_H_

#include <cstddef>
#include <string_view>

namespace ihs::log {

/**
 * @brief Emit a message one line at a time.
 *
 * Dart messages — exception stack traces above all — arrive as several lines
 * in one string, and a log record is a line. Splitting on newlines here keeps
 * one stack frame per record instead of running them together.
 *
 * Length is deliberately not this function's problem. A run with no newline to
 * break on is emitted whole, however long: the logging library splits an
 * over-length message across records itself and the drain rejoins them, so it
 * reaches a sink intact. Cutting it here would hand the library several
 * complete messages instead, and it would print as several stamped pieces.
 *
 * An empty message is emitted once, unchanged: a caller logging "" means to
 * produce a line.
 */
template <class Emit>
void emit_chunked(const char* message, Emit emit) {
  if (message == nullptr) {
    return;
  }
  std::string_view rest{message};
  if (rest.empty()) {
    emit(rest);
    return;
  }
  while (!rest.empty()) {
    std::size_t take = rest.size();
    const std::size_t nl = rest.find('\n');
    const bool at_newline = nl != std::string_view::npos && nl < take;
    if (at_newline) {
      take = nl;
    }
    emit(rest.substr(0, take));
    rest.remove_prefix(at_newline ? take + 1 : take);  // consume the '\n' too
  }
}

}  // namespace ihs::log

#endif  // SHELL_LOGGING_LOG_CHUNK_H_
