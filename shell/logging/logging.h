/*
 * Copyright 2020 Toyota Connected North America
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

#pragma once

// Shell logging surface. Formats a line with fmt ("{}" style) in the calling
// thread, then hands the finished text to the ihs_shared logging C ABI
// (ihs_log), which fans it out to the configured sinks (console / file / DLT,
// selected by IHS_LOG_SINK). spdlog is retired: this header is the single
// replacement for the old console logger and the DLT callback sink alike.
//
// fmt is sourced header-only from the standalone fmt submodule; the formatting
// happens shell-side, before the C ABI, so libihs_shared.so stays dependency
// free. Levels are filtered cheaply via ihs_log_enabled() before any formatting
// or argument evaluation happens.

#ifndef FMT_HEADER_ONLY
#define FMT_HEADER_ONLY
#endif
// fmt/format.h defines detail::allocator, which calls malloc and free
// without declaring them. It relied on some other header providing
// <cstdlib> transitively; libc++ 23 no longer does, and the calls are
// then neither visible at the point of definition nor reachable by ADL:
//
//   fmt/format.h:747:28: error: use of undeclared identifier 'malloc'
//   fmt/format.h:752:35: error: call to function 'free' that is neither
//     visible in the template definition nor found by argument-dependent lookup
//
// Include it before fmt so the declarations are in scope there. Upstream
// fmt has since dropped that allocator, but no released version has.
#include <cstdlib>

#include <fmt/format.h>

#include "ihs/logging.h"
#include "logging/logger.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

namespace ihs::log {

// Process-wide default context for shell log lines. Normally opened on the
// first log after IHS_LOGGING_START (which main() calls before anything logs).
// If the first use happens before start, the open returns an invalid handle;
// rather than cache that permanently (which would silently wedge all logging),
// re-attempt while invalid so logging recovers once the bridge is up. The
// pre-start window is single-threaded (no producers exist before start), so the
// re-open needs no synchronization.
inline IhsLogContext& default_context() {
  static IhsLogContext ctx("SHEL", "ivi-homescreen shell");
  if (!ctx.is_valid()) {
    ctx = IhsLogContext("SHEL", "ivi-homescreen shell");
  }
  return ctx;
}

namespace detail {

inline bool passes(IhsLogContext& ctx, IhsLogLevel level) {
  return ctx.is_valid() &&
         ihs_log_enabled(ctx.index(), static_cast<std::uint8_t>(level)) != 0;
}

// spdlog was configured flush_on(err): an error or critical line was written
// synchronously, so it appeared with no drain delay and survived a subsequent
// abort()/crash. Match that here — flush after an error-or-worse record.
// IhsLogLevel is numerically ascending by verbosity (FATAL=1, ERROR=2), so
// "error or worse" is level <= ERROR. Info/debug/verbose stay async.
inline void maybe_flush(IhsLogLevel level) {
  if (level <= IHS_LEVEL_ERROR) {
    ihs_log_flush();
  }
}

// Verbatim line: emitted as-is, with no format parsing — safe for a runtime
// string that may itself contain '{'/'}' and matches spdlog's single-argument
// (non-formatting) behavior.
inline void emit_raw(IhsLogLevel level, std::string_view msg) {
  IhsLogContext& ctx = default_context();
  if (!passes(ctx, level)) {
    return;
  }
  ihs_log(ctx.index(), static_cast<std::uint8_t>(level), msg.data(),
          msg.size());
  maybe_flush(level);
}

// Formatted line: fmt "{}" into a stack buffer. The ihs_log_enabled() gate
// runs first, so a level filtered out by IHS_LOG_LEVEL costs nothing beyond
// the gate — no formatting, no argument evaluation.
template <class... Args>
inline void emit_fmt(IhsLogLevel level,
                     fmt::format_string<Args...> fmt_str,
                     Args&&... args) {
  IhsLogContext& ctx = default_context();
  if (!passes(ctx, level)) {
    return;
  }
  char buf[IHS_LOG_TEXT_CAPACITY];
  const auto result = fmt::format_to_n(buf, sizeof(buf) - 1, fmt_str,
                                       std::forward<Args>(args)...);
  if (result.size < sizeof(buf)) {
    ihs_log(ctx.index(), static_cast<std::uint8_t>(level), buf,
            static_cast<std::size_t>(result.size));
    maybe_flush(level);
    return;
  }
  // Too long for the stack buffer. Format it again into a growable one and
  // hand the whole thing over: ihs_log splits an over-length message across
  // records and the drain rejoins them, so the line survives -- but only if it
  // arrives intact. Clipping it here would lose the tail before the library
  // ever sees it, silently, unlike a record the library had to drop.
  //
  // The buffer is thread_local and kept, so a caller that logs long lines
  // repeatedly allocates once rather than per call, and a caller that never
  // does pays nothing.
  static thread_local std::string overflow;
  overflow.clear();
  fmt::format_to(std::back_inserter(overflow), fmt_str,
                 std::forward<Args>(args)...);
  ihs_log(ctx.index(), static_cast<std::uint8_t>(level), overflow.data(),
          overflow.size());
  maybe_flush(level);
}

}  // namespace detail

// Each level has two overloads: a verbatim form (a complete line, or a runtime
// string) and a compile-time-checked formatted form (a literal + >=1 argument).
// The one-argument call resolves to the verbatim overload, so a bare message —
// literal or runtime — never goes through the formatter.
#define IHS_LOG_DEFINE_LEVEL(name_, level_)                               \
  inline void name_(std::string_view msg) {                               \
    detail::emit_raw((level_), msg);                                      \
  }                                                                       \
  template <class Arg0, class... Args>                                    \
  inline void name_(fmt::format_string<Arg0, Args...> fmt_str, Arg0&& a0, \
                    Args&&... a) {                                        \
    detail::emit_fmt((level_), fmt_str, std::forward<Arg0>(a0),           \
                     std::forward<Args>(a)...);                           \
  }

IHS_LOG_DEFINE_LEVEL(trace, IHS_LEVEL_VERBOSE)
IHS_LOG_DEFINE_LEVEL(debug, IHS_LEVEL_DEBUG)
IHS_LOG_DEFINE_LEVEL(info, IHS_LEVEL_INFO)
IHS_LOG_DEFINE_LEVEL(warn, IHS_LEVEL_WARN)
IHS_LOG_DEFINE_LEVEL(error, IHS_LEVEL_ERROR)
IHS_LOG_DEFINE_LEVEL(critical, IHS_LEVEL_FATAL)
#undef IHS_LOG_DEFINE_LEVEL

}  // namespace ihs::log

// Convenience aliases kept for existing call sites.
#define LOG_INFO ihs::log::info
#define LOG_DEBUG ihs::log::debug
#define LOG_WARN ihs::log::warn
#define LOG_ERROR ihs::log::error
#define LOG_TRACE ihs::log::trace
#define LOG_CRITICAL ihs::log::critical

// Diagnostic debug/trace: compiled out entirely in release builds, as the old
// SPDLOG_DEBUG / SPDLOG_TRACE were (SPDLOG_ACTIVE_LEVEL=OFF under NDEBUG).
#ifndef NDEBUG
#define IHS_DEBUG(...) ihs::log::debug(__VA_ARGS__)
#define IHS_TRACE(...) ihs::log::trace(__VA_ARGS__)
#else
#define IHS_DEBUG(...) ((void)0)
#define IHS_TRACE(...) ((void)0)
#endif
