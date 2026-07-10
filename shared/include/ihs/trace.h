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

/*
 * ihs_shared tracing surface (C ABI). Emits duration / instant / counter /
 * flow events to, in priority order: the Flutter engine trace procs (once the
 * shell installs them), then the kernel ftrace trace_marker (so backend
 * bring-up before the engine is still captured), then nothing. Emitting is
 * gated by a single relaxed-atomic load so a disabled trace is near-free.
 * See docs/PLUGIN_ABI.md.
 */

#ifndef IHS_TRACE_H_
#define IHS_TRACE_H_

#include <stddef.h>
#include <stdint.h>

#include "ihs/ihs_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The Flutter engine trace procs, installed by the shell once the engine proc
 * table resolves. The embedder trace API is name-based (duration + instant);
 * counters and flows have no engine equivalent and fall back to trace_marker.
 */
typedef struct IhsTraceEngineProcs {
  size_t struct_size;
  void (*duration_begin)(const char* name);
  void (*duration_end)(const char* name);
  void (*instant)(const char* name);
} IhsTraceEngineProcs;

/*
 * Install (or clear, with NULL) the engine trace sink. Shell-only; called once
 * the engine proc table is up. The pointer must remain valid for the process
 * lifetime.
 */
IHS_EXPORT void ihs_trace_set_engine_procs(const IhsTraceEngineProcs* procs);

/* Non-zero when a sink is active. A relaxed load — cheap enough to gate every
 * trace site. */
IHS_EXPORT int ihs_trace_enabled(void);

/* Emit entry points. Duration begin/end nest by name on the calling thread. */
IHS_EXPORT void ihs_trace_duration_begin(const char* name);
IHS_EXPORT void ihs_trace_duration_end(const char* name);
IHS_EXPORT void ihs_trace_instant(const char* name);
IHS_EXPORT void ihs_trace_counter(const char* name, int64_t value);
IHS_EXPORT void ihs_trace_flow_begin(const char* name, int64_t id);
IHS_EXPORT void ihs_trace_flow_step(const char* name, int64_t id);
IHS_EXPORT void ihs_trace_flow_end(const char* name, int64_t id);

/* Trace capability sub-table (IhsApi::trace). */
typedef struct IhsTraceApi {
  size_t struct_size;
  int (*enabled)(void);
  void (*duration_begin)(const char* name);
  void (*duration_end)(const char* name);
  void (*instant)(const char* name);
  void (*counter)(const char* name, int64_t value);
  void (*flow_begin)(const char* name, int64_t id);
  void (*flow_step)(const char* name, int64_t id);
  void (*flow_end)(const char* name, int64_t id);
  void (*set_engine_procs)(const IhsTraceEngineProcs* procs);
} IhsTraceApi;

/*
 * Zero-cost-when-disabled macros: each gates its call on ihs_trace_enabled().
 */
#define IHS_TRACE_BEGIN(name_)           \
  do {                                   \
    if (ihs_trace_enabled())             \
      ihs_trace_duration_begin((name_)); \
  } while (0)
#define IHS_TRACE_END(name_)           \
  do {                                 \
    if (ihs_trace_enabled())           \
      ihs_trace_duration_end((name_)); \
  } while (0)
#define IHS_TRACE_INSTANT(name_)  \
  do {                            \
    if (ihs_trace_enabled())      \
      ihs_trace_instant((name_)); \
  } while (0)
#define IHS_TRACE_COUNTER(name_, val_)    \
  do {                                    \
    if (ihs_trace_enabled())              \
      ihs_trace_counter((name_), (val_)); \
  } while (0)
#define IHS_TRACE_FLOW_BEGIN(name_, id_)    \
  do {                                      \
    if (ihs_trace_enabled())                \
      ihs_trace_flow_begin((name_), (id_)); \
  } while (0)
#define IHS_TRACE_FLOW_STEP(name_, id_)    \
  do {                                     \
    if (ihs_trace_enabled())               \
      ihs_trace_flow_step((name_), (id_)); \
  } while (0)
#define IHS_TRACE_FLOW_END(name_, id_)    \
  do {                                    \
    if (ihs_trace_enabled())              \
      ihs_trace_flow_end((name_), (id_)); \
  } while (0)

#ifdef __cplusplus
} /* extern "C" */

namespace ihs {

// RAII duration scope. Captures the name at construction so begin/end pair even
// if the enable flag flips mid-scope.
class TraceScope {
 public:
  explicit TraceScope(const char* name) {
    if (ihs_trace_enabled()) {
      name_ = name;
      ihs_trace_duration_begin(name);
    }
  }
  ~TraceScope() {
    if (name_ != nullptr) {
      ihs_trace_duration_end(name_);
    }
  }
  TraceScope(const TraceScope&) = delete;
  TraceScope& operator=(const TraceScope&) = delete;

 private:
  const char* name_ = nullptr;
};

}  // namespace ihs

#define IHS_TRACE_SCOPE_CONCAT_(a, b) a##b
#define IHS_TRACE_SCOPE_NAME_(line) \
  IHS_TRACE_SCOPE_CONCAT_(ihs_trace_scope_, line)
#define IHS_TRACE_SCOPE(name_) \
  ::ihs::TraceScope IHS_TRACE_SCOPE_NAME_(__LINE__)(name_)

#endif /* __cplusplus */

#endif /* IHS_TRACE_H_ */
