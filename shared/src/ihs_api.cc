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

#include "ihs/ihs.h"

#include <string>

namespace {

// Per-thread description of the last failure, surfaced through
// ihs_last_error_message(). Thread-local so a diagnostic on one thread cannot
// be clobbered by a call on another.
thread_local std::string g_last_error;

// The process-lifetime capability table. Sub-table pointers are null until the
// corresponding surface is compiled into the library; a consumer treats a null
// sub-table as "capability absent" (see docs/PLUGIN_ABI.md).
const IhsApi g_api = {
    /* struct_size   */ sizeof(IhsApi),
    /* abi_version   */ IHS_SHARED_ABI_VERSION,
    /* logging       */ nullptr,
    /* trace         */ nullptr,
    /* platform_view */ nullptr,
    /* config        */ nullptr,
};

}  // namespace

extern "C" const IhsApi* ihs_get_api(uint32_t requested_abi) {
  const uint32_t requested_major = IHS_ABI_MAJOR(requested_abi);
  if (requested_major != IHS_SHARED_ABI_MAJOR) {
    g_last_error = "ihs_get_api: unsupported ABI major " +
                   std::to_string(requested_major) +
                   "; this library provides " +
                   std::to_string(IHS_SHARED_ABI_MAJOR);
    return nullptr;
  }
  g_last_error.clear();
  return &g_api;
}

extern "C" const char* ihs_last_error_message(void) {
  return g_last_error.c_str();
}
