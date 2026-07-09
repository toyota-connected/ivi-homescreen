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

#include "ihs/trace.h"

#include "ihs_internal.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

// Installed engine sink (highest priority). Loaded with acquire on the hot
// path; stored with release when the shell registers it.
std::atomic<const IhsTraceEngineProcs*> g_engine{nullptr};

// ftrace trace_marker fallback: opened once at library load, then read-only.
// Never closed — the library is pinned with -z nodelete for the process life.
int g_marker_fd = -1;
int g_pid = 0;

// Single coarse enable flag the macros gate on: set when either sink exists.
std::atomic<int> g_enabled{0};

struct MarkerInit {
  MarkerInit() {
    g_pid = static_cast<int>(::getpid());
    static const char* const kPaths[] = {
        "/sys/kernel/tracing/trace_marker",
        "/sys/kernel/debug/tracing/trace_marker",
    };
    for (const char* path : kPaths) {
      const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
      if (fd >= 0) {
        g_marker_fd = fd;
        break;
      }
    }
    if (g_marker_fd >= 0) {
      g_enabled.store(1, std::memory_order_relaxed);
    }
  }
};
MarkerInit g_marker_init;

// Write a snprintf'd record. `n` is snprintf's return (the length it WOULD
// have produced), so it is clamped to the buffer to avoid an out-of-bounds
// read when a long name truncated the output.
void write_marker(const char* buf, int n, size_t cap) {
  if (g_marker_fd < 0 || n <= 0) {
    return;
  }
  const size_t len =
      static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
  const ssize_t rc = ::write(g_marker_fd, buf, len);
  (void)rc;  // best-effort; a full/again trace buffer is not our concern
}

}  // namespace

extern "C" void ihs_trace_set_engine_procs(const IhsTraceEngineProcs* procs) {
  g_engine.store(procs, std::memory_order_release);
  const bool on = procs != nullptr || g_marker_fd >= 0;
  g_enabled.store(on ? 1 : 0, std::memory_order_relaxed);
}

extern "C" int ihs_trace_enabled(void) {
  return g_enabled.load(std::memory_order_relaxed);
}

extern "C" void ihs_trace_duration_begin(const char* name) {
  if (name == nullptr) {
    return;
  }
  if (const auto* e = g_engine.load(std::memory_order_acquire)) {
    e->duration_begin(name);
    return;
  }
  char buf[256];
  const int n = std::snprintf(buf, sizeof(buf), "B|%d|%s", g_pid, name);
  write_marker(buf, n, sizeof(buf));
}

extern "C" void ihs_trace_duration_end(const char* name) {
  if (const auto* e = g_engine.load(std::memory_order_acquire)) {
    e->duration_end(name != nullptr ? name : "");
    return;
  }
  char buf[64];
  const int n = std::snprintf(buf, sizeof(buf), "E|%d", g_pid);
  write_marker(buf, n, sizeof(buf));
}

extern "C" void ihs_trace_instant(const char* name) {
  if (name == nullptr) {
    return;
  }
  if (const auto* e = g_engine.load(std::memory_order_acquire)) {
    e->instant(name);
    return;
  }
  char buf[256];
  const int n = std::snprintf(buf, sizeof(buf), "I|%d|%s", g_pid, name);
  write_marker(buf, n, sizeof(buf));
}

extern "C" void ihs_trace_counter(const char* name, int64_t value) {
  if (name == nullptr) {
    return;
  }
  // No engine equivalent; counters go to trace_marker when available.
  char buf[256];
  const int n = std::snprintf(buf, sizeof(buf), "C|%d|%s|%lld", g_pid, name,
                              static_cast<long long>(value));
  write_marker(buf, n, sizeof(buf));
}

namespace {

// atrace async form: S starts, F finishes; a step has no standard marker, so it
// is emitted as an instant-style tag. Readers that don't parse it still see the
// text. Only used on the trace_marker fallback (the engine API has no flows).
void flow_marker(char tag, const char* name, int64_t id) {
  if (name == nullptr) {
    return;
  }
  char buf[256];
  const int n = std::snprintf(buf, sizeof(buf), "%c|%d|%s|%lld", tag, g_pid,
                              name, static_cast<long long>(id));
  write_marker(buf, n, sizeof(buf));
}

}  // namespace

extern "C" void ihs_trace_flow_begin(const char* name, int64_t id) {
  flow_marker('S', name, id);
}

extern "C" void ihs_trace_flow_step(const char* name, int64_t id) {
  flow_marker('T', name, id);
}

extern "C" void ihs_trace_flow_end(const char* name, int64_t id) {
  flow_marker('F', name, id);
}

namespace ihs::trace {

const IhsTraceApi* trace_api() noexcept {
  static const IhsTraceApi api = {
      sizeof(IhsTraceApi),       &ihs_trace_enabled,
      &ihs_trace_duration_begin, &ihs_trace_duration_end,
      &ihs_trace_instant,        &ihs_trace_counter,
      &ihs_trace_flow_begin,     &ihs_trace_flow_step,
      &ihs_trace_flow_end,       &ihs_trace_set_engine_procs,
  };
  return &api;
}

}  // namespace ihs::trace
