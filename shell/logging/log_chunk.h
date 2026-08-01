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
 * @brief Split a message so no piece exceeds @p max bytes, emitting each.
 *
 * A single ihs log record is capped at IHS_LOG_TEXT_CAPACITY bytes, but Dart
 * messages — exception stack traces, an encoded summary — routinely exceed
 * that, and the tail is where a structured payload keeps what the reader came
 * for. Breaking on newlines first keeps a stack frame per record; whatever is
 * still over length is hard-split rather than dropped.
 *
 * @p max is the budget for the text handed to @p emit, so a caller that
 * prefixes each record (with a tag, say) passes the record capacity minus that
 * prefix. It is clamped to at least 1 so a pathological budget cannot spin.
 *
 * An empty message is emitted once, unchanged: a caller logging "" means to
 * produce a line.
 */
template <class Emit>
void emit_chunked(const char* message, std::size_t max, Emit emit) {
  if (message == nullptr) {
    return;
  }
  if (max == 0) {
    max = 1;
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
    if (take > max) {
      take = max;
      emit(rest.substr(0, take));
      rest.remove_prefix(take);
      continue;
    }
    emit(rest.substr(0, take));
    rest.remove_prefix(at_newline ? take + 1 : take);  // consume the '\n' too
  }
}

}  // namespace ihs::log

#endif  // SHELL_LOGGING_LOG_CHUNK_H_
