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

#include "ihs_internal.hpp"

namespace {

// Per-thread description of the last failure, surfaced through
// ihs_last_error_message(). Thread-local so a diagnostic on one thread cannot
// be clobbered by a call on another.
thread_local std::string g_last_error;

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

  // The process-lifetime capability table, built on first call so the
  // sub-table pointers resolve at runtime (avoiding a static-initialization
  // order dependency between translation units). A sub-table is null when the
  // corresponding surface is not compiled into the library; a consumer treats
  // that as "capability absent" (see docs/PLUGIN_ABI.md).
  static const IhsApi api = {
      /* struct_size   */ sizeof(IhsApi),
      /* abi_version   */ IHS_SHARED_ABI_VERSION,
      /* logging       */ ihs::dlt::logging_api(),
      /* trace         */ ihs::trace::trace_api(),
      /* platform_view */ nullptr,
      /* config        */ ihs::config::config_api(),
  };
  return &api;
}

extern "C" const char* ihs_last_error_message(void) {
  return g_last_error.c_str();
}
