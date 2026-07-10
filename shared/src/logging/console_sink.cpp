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

#include "console_sink.hpp"

#include <unistd.h>

#include <cstdio>
#include <ctime>

namespace ihs::dlt {

namespace {

char level_char(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Fatal:
      return 'F';
    case LogLevel::Error:
      return 'E';
    case LogLevel::Warn:
      return 'W';
    case LogLevel::Info:
      return 'I';
    case LogLevel::Debug:
      return 'D';
    case LogLevel::Verbose:
      return 'V';
    case LogLevel::Off:
    default:
      return '?';
  }
}

// ANSI SGR color per severity; empty when uncolored.
const char* level_color(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Fatal:
    case LogLevel::Error:
      return "\033[31m";  // red
    case LogLevel::Warn:
      return "\033[33m";  // yellow
    case LogLevel::Debug:
    case LogLevel::Verbose:
      return "\033[2m";  // dim
    default:
      return "";  // Info: default color
  }
}

}  // namespace

ConsoleSink::ConsoleSink() noexcept : color_(::isatty(STDERR_FILENO) != 0) {}

void ConsoleSink::write(const LogRecord& record) noexcept {
  const auto secs = static_cast<std::time_t>(record.ts_ns / 1'000'000'000ULL);
  const auto msec = static_cast<unsigned>((record.ts_ns / 1'000'000ULL) % 1000);
  std::tm tm{};
  ::localtime_r(&secs, &tm);
  char ts[16];
  std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03u", tm.tm_hour, tm.tm_min,
                tm.tm_sec, msec);

  const char* tag = record.tag != nullptr ? record.tag : "";
  if (color_) {
    const char* color = level_color(record.level);
    std::fprintf(stderr, "%s%s [%c] %s: %s\033[0m\n", color, ts,
                 level_char(record.level), tag, record.text);
  } else {
    std::fprintf(stderr, "%s [%c] %s: %s\n", ts, level_char(record.level), tag,
                 record.text);
  }
}

void ConsoleSink::flush() noexcept {
  std::fflush(stderr);
}

}  // namespace ihs::dlt
