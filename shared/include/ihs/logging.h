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
 * ihs_shared logging surface: a generic C-ABI logging interface. Producers open
 * a context (tag) and emit pre-formatted lines; the records fan out to whatever
 * sinks are configured. DLT is one sink, not the interface — sinks are selected
 * by environment (IHS_LOG_SINK = dlt|console|file, IHS_LOG_LEVEL, IHS_LOG_FILE,
 * IHS_LOG_FILE_MAX_BYTES, IHS_LOG_FILE_MAX_FILES). See docs/PLUGIN_ABI.md.
 *
 * Timing contract: ihs_log() is non-blocking (enqueue to a per-thread ring;
 * when the ring is full the incoming record is dropped and a per-ring counter
 * is bumped); formatting and I/O happen on a drain thread. ihs_log_flush() is
 * synchronous.
 */

#ifndef IHS_LOGGING_H_
#define IHS_LOGGING_H_

#include <stddef.h>
#include <stdint.h>

#include "ihs/ihs_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Severity, matching the DLT log-level scale (numerically larger = more
 * verbose). IHS_LOG_LEVEL sets a floor; more-verbose records are dropped. */
typedef enum IhsLogLevel {
  IHS_LEVEL_OFF = 0,
  IHS_LEVEL_FATAL = 1,
  IHS_LEVEL_ERROR = 2,
  IHS_LEVEL_WARN = 3,
  IHS_LEVEL_INFO = 4,
  IHS_LEVEL_DEBUG = 5,
  IHS_LEVEL_VERBOSE = 6
} IhsLogLevel;

/*
 * Maximum text length carried in one log record; longer messages are
 * truncated. A convenience wrapper sizing a stack buffer for a formatted line
 * should use this.
 */
#define IHS_LOG_TEXT_CAPACITY 240

/*
 * Options for ihs_log_context_open. struct_size-first; all fields optional
 * (pass NULL for defaults). Sink-specific hints live here rather than in the
 * call signature, so the core interface stays sink-neutral.
 */
typedef struct IhsLogContextOptions {
  size_t struct_size;
  const char* description; /* human-readable context description */
  char sink_context_id[4]; /* optional explicit 4-char context id for sinks
                              that key on one (e.g. the DLT context id);
                              derived from the tag when left zeroed */
} IhsLogContextOptions;

/*
 * Bring logging online: register the app id, resolve sinks from the
 * environment, start the drain thread. Called by the shell before any plugin
 * is loaded. Idempotent — only the first successful call takes effect. Returns
 * 1 on success, 0 otherwise.
 */
IHS_EXPORT int ihs_log_start(const char* app_id, const char* description);

/* Tear logging down. Intended for the shell at process shutdown. */
IHS_EXPORT void ihs_log_stop(void);

/* Synchronously flush pending records to the active sinks. */
IHS_EXPORT void ihs_log_flush(void);

/*
 * Open (or look up) a logging context for tag. Returns a non-negative index for
 * use with ihs_log, or -1 on failure. options may be NULL. Contexts are cached;
 * re-opening the same tag is cheap.
 */
IHS_EXPORT int32_t ihs_log_context_open(const char* tag,
                                        const IhsLogContextOptions* options);

/*
 * Emit a pre-formatted line under a context. level is an IhsLogLevel. Enqueue
 * is wait-free and drops silently on ring overflow or when level is below the
 * IHS_LOG_LEVEL floor. Returns 1 when enqueued, 0 on drop/invalid arguments.
 */
IHS_EXPORT int ihs_log(int32_t ctx_index,
                       uint8_t level,
                       const char* text,
                       size_t text_len);

/*
 * The logging capability sub-table reachable through IhsApi::logging. The
 * function pointers alias the flat entry points above; a consumer may use
 * either. Grows additively behind struct_size.
 */
typedef struct IhsLoggingApi {
  size_t struct_size;
  int (*start)(const char* app_id, const char* description);
  void (*stop)(void);
  void (*flush)(void);
  int32_t (*context_open)(const char* tag, const IhsLogContextOptions* options);
  int (*log)(int32_t ctx_index, uint8_t level, const char* text, size_t len);
} IhsLoggingApi;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_LOGGING_H_ */
