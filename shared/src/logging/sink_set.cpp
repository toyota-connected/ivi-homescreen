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

#include "sink_set.hpp"

#include "console_sink.hpp"
#include "file_sink.hpp"
#if ENABLE_DLT
#include "dlt_sink.hpp"
#include "libdlt_loader.hpp"
#endif

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace ihs::dlt {

namespace {

constexpr std::size_t kDefaultFileMaxBytes = 5 * 1024 * 1024;  // 5 MiB
constexpr int kDefaultFileMaxFiles = 3;
// Ceiling on IHS_LOG_FILE_MAX_FILES: the rotation cascade renames every kept
// file on each roll, so an unbounded value would stall the single drain thread
// (and hence all logging) on its first rotation.
constexpr int kMaxFileMaxFiles = 1000;

std::string_view getenv_sv(const char* name) noexcept {
  const char* v = std::getenv(name);
  return v != nullptr ? std::string_view{v} : std::string_view{};
}

// Parse IHS_LOG_LEVEL: a name (case-insensitive) or a 0-6 number. Unset/invalid
// leaves the default (pass everything).
LogLevel parse_level(std::string_view spec, LogLevel fallback) noexcept {
  if (spec.empty()) {
    return fallback;
  }
  struct {
    std::string_view name;
    LogLevel level;
  } static const kNames[] = {
      {"off", LogLevel::Off},         {"fatal", LogLevel::Fatal},
      {"error", LogLevel::Error},     {"warn", LogLevel::Warn},
      {"info", LogLevel::Info},       {"debug", LogLevel::Debug},
      {"verbose", LogLevel::Verbose},
  };
  for (const auto& n : kNames) {
    if (spec.size() == n.name.size() &&
        ::strncasecmp(spec.data(), n.name.data(), n.name.size()) == 0) {
      return n.level;
    }
  }
  if (spec.size() == 1 && spec[0] >= '0' && spec[0] <= '6') {
    return static_cast<LogLevel>(spec[0] - '0');
  }
  return fallback;
}

std::size_t parse_size(std::string_view spec, std::size_t fallback) noexcept {
  if (spec.empty()) {
    return fallback;
  }
  // strtoull silently wraps a leading '-' (e.g. "-1" -> ULLONG_MAX), which
  // would disable rotation and let the log grow without bound; reject it
  // outright.
  if (spec.front() == '-') {
    return fallback;
  }
  char* end = nullptr;
  const unsigned long long v =
      std::strtoull(std::string(spec).c_str(), &end, 10);
  // Guard the size_t cast: a value that does not fit (only possible where
  // size_t is 32-bit) would truncate to something tiny and rotate every line.
  if (end == nullptr || *end != '\0' || v == 0 ||
      v > std::numeric_limits<std::size_t>::max()) {
    return fallback;
  }
  return static_cast<std::size_t>(v);
}

int parse_int(std::string_view spec, int fallback) noexcept {
  if (spec.empty()) {
    return fallback;
  }
  char* end = nullptr;
  const long v = std::strtol(std::string(spec).c_str(), &end, 10);
  return (end != nullptr && *end == '\0' && v > 0) ? static_cast<int>(v)
                                                   : fallback;
}

void warn_once(const char* message) noexcept {
  std::fprintf(stderr, "[ihs_log] %s\n", message);
}

}  // namespace

SinkSet build_sink_set_from_env() {
  SinkSet set;
  set.level_floor = parse_level(getenv_sv("IHS_LOG_LEVEL"), LogLevel::Verbose);

  std::string_view spec = getenv_sv("IHS_LOG_SINK");
  if (spec.empty()) {
#if ENABLE_DLT
    spec = "dlt";  // default: the primary target when built with DLT
#else
    spec = "console";  // no DLT compiled in; console is the sensible default
#endif
  }

  bool want_console = false;

  // Comma-separated token walk (no allocation of a token vector).
  std::size_t pos = 0;
  while (pos < spec.size()) {
    std::size_t comma = spec.find(',', pos);
    if (comma == std::string_view::npos) {
      comma = spec.size();
    }
    std::string_view tok = spec.substr(pos, comma - pos);
    pos = comma + 1;
    while (!tok.empty() && tok.front() == ' ') {
      tok.remove_prefix(1);
    }
    while (!tok.empty() && tok.back() == ' ') {
      tok.remove_suffix(1);
    }
    if (tok.empty()) {
      continue;
    }

    if (tok == "dlt") {
#if ENABLE_DLT
      LibDltLoader& loader = LibDltLoader::instance();
      if (loader.available()) {
        set.sinks.push_back(std::make_unique<DltSink>(loader));
      } else {
        warn_once("IHS_LOG_SINK=dlt but libdlt is unavailable; using console");
        want_console = true;
      }
#else
      warn_once(
          "IHS_LOG_SINK=dlt but this build has no DLT support; using console");
      want_console = true;
#endif
    } else if (tok == "console") {
      want_console = true;
    } else if (tok == "file") {
      const std::string_view path = getenv_sv("IHS_LOG_FILE");
      if (path.empty()) {
        warn_once("IHS_LOG_SINK=file but IHS_LOG_FILE is unset; using console");
        want_console = true;
      } else {
        auto file = RotatingFileSink::Create(
            std::string(path),
            parse_size(getenv_sv("IHS_LOG_FILE_MAX_BYTES"),
                       kDefaultFileMaxBytes),
            std::min(parse_int(getenv_sv("IHS_LOG_FILE_MAX_FILES"),
                               kDefaultFileMaxFiles),
                     kMaxFileMaxFiles));
        if (file) {
          set.sinks.push_back(std::move(file));
        } else {
          warn_once("IHS_LOG_FILE could not be opened; using console");
          want_console = true;
        }
      }
    } else {
      warn_once("unknown IHS_LOG_SINK token; ignoring");
    }
  }

  // Never log silently to nowhere: fall back to console when nothing else took.
  if (want_console || set.sinks.empty()) {
    set.sinks.push_back(std::make_unique<ConsoleSink>());
  }
  return set;
}

}  // namespace ihs::dlt
