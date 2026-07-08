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
 * ihs_shared: the C ABI entry point for out-of-tree ivi-homescreen FFI
 * plugins. See docs/PLUGIN_ABI.md for the boundary contract.
 */

#ifndef IHS_IHS_H_
#define IHS_IHS_H_

#include <stddef.h>
#include <stdint.h>

#include "ihs/ihs_export.h"
#include "ihs/ihs_version.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Capability sub-tables. Each is defined by its own header once the
 * corresponding surface is present; the pointer in IhsApi is NULL in a build
 * that does not provide the capability.
 */
typedef struct IhsLoggingApi IhsLoggingApi;
typedef struct IhsTraceApi IhsTraceApi;
typedef struct IhsPlatformViewApi IhsPlatformViewApi;
typedef struct IhsConfigApi IhsConfigApi;

/*
 * The top-level capability table, valid for the process lifetime. struct_size
 * is first and additive growth only, so a plugin built against an older minor
 * safely reads a prefix of a newer table.
 */
typedef struct IhsApi {
  size_t struct_size;
  uint32_t abi_version;
  const IhsLoggingApi* logging;
  const IhsTraceApi* trace;
  const IhsPlatformViewApi* platform_view;
  const IhsConfigApi* config;
} IhsApi;

/*
 * Returns the capability table, or NULL if requested_abi's major version is
 * not the one this library provides (query ihs_last_error_message() for the
 * reason). requested_abi is normally IHS_SHARED_ABI_VERSION of the header the
 * plugin was built against.
 */
IHS_EXPORT const IhsApi* ihs_get_api(uint32_t requested_abi);

/*
 * A human-readable description of the last failure on the calling thread.
 * Never NULL; the empty string when there has been no failure. The returned
 * pointer is valid until the next ihs_* call on the same thread.
 */
IHS_EXPORT const char* ihs_last_error_message(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_IHS_H_ */
