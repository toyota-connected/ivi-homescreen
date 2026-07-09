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
 * ihs_shared logging surface: a Diagnostic Log and Trace (DLT) bridge exposed
 * as a C ABI. The shell brings the bridge online before any plugin loads;
 * plugins acquire contexts and emit pre-formatted lines. See
 * docs/PLUGIN_ABI.md.
 */

#ifndef IHS_LOGGING_H_
#define IHS_LOGGING_H_

#include <stddef.h>
#include <stdint.h>

#include "ihs/ihs_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Severity, matching the DLT log-level scale. */
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
 * truncated by the bridge. A convenience wrapper sizing a stack buffer for a
 * formatted line should use this.
 */
#define IHS_LOG_TEXT_CAPACITY 240

/*
 * Bring the bridge online: load libdlt, register the app id, start the worker
 * thread. Called by the shell before any plugin is loaded. Idempotent — only
 * the first successful call takes effect, so a later call from a plugin is a
 * no-op rather than a re-initialization. Returns 1 on success, 0 on failure.
 */
IHS_EXPORT int ihs_dlt_start(const char* app_id, const char* description);

/* Tear the bridge down. Intended for the shell at process shutdown. */
IHS_EXPORT void ihs_dlt_stop(void);

/* Flush pending records to the DLT daemon. */
IHS_EXPORT void ihs_dlt_flush(void);

/*
 * Acquire (or look up) a logging context. Returns a non-negative index for use
 * with ihs_dlt_log, or -1 on failure. Contexts are cached; re-acquiring the
 * same ctx_id is cheap.
 */
IHS_EXPORT int32_t ihs_dlt_acquire_context(const char* ctx_id,
                                           const char* description);

/*
 * Emit a pre-formatted line under a context. level is an IhsLogLevel. The hot
 * path is wait-free and drops silently on ring overflow. Returns 1 when the
 * record was enqueued, 0 on drop or invalid arguments.
 */
IHS_EXPORT int ihs_dlt_log(int32_t ctx_index,
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
  int32_t (*acquire_context)(const char* ctx_id, const char* description);
  int (*log)(int32_t ctx_index, uint8_t level, const char* text, size_t len);
} IhsLoggingApi;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_LOGGING_H_ */
