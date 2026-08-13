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
 * Internal (non-installed) declarations shared between the ihs_shared entry
 * point and the capability implementations. Not part of the plugin ABI.
 */

#ifndef IHS_SHARED_SRC_IHS_INTERNAL_HPP_
#define IHS_SHARED_SRC_IHS_INTERNAL_HPP_

#include "ihs/config.h"
#include "ihs/logging.h"
#include "ihs/platform_view.h"
#include "ihs/trace.h"

#ifdef IHS_WITH_SEMANTICS
#include "ihs/ihs_semantics.h"
#endif

namespace ihs::dlt {

// Returns the logging sub-table (function pointers over the flat ihs_dlt_*
// entry points). Defined in logging/ffi_shim.cpp; compiled only when the DLT
// bridge is built into the library.
const IhsLoggingApi* logging_api() noexcept;

}  // namespace ihs::dlt

namespace ihs::trace {

// Returns the tracing sub-table. Defined in trace.cc; always compiled in.
const IhsTraceApi* trace_api() noexcept;

}  // namespace ihs::trace

namespace ihs::config {

// Returns the config read sub-table. Defined in config.cc; always compiled in.
const IhsConfigApi* config_api() noexcept;

}  // namespace ihs::config

namespace ihs::pv {

// Returns the platform-view sub-table (function pointers over the flat ihs_pv_*
// entry points). Defined in platform_view.cc; always compiled in.
const IhsPlatformViewApi* platform_view_api() noexcept;

}  // namespace ihs::pv

#ifdef IHS_WITH_SEMANTICS
namespace ihs::semantics {

// Returns the semantics hub sub-table. Defined in semantics.cc, which is
// compiled only with BUILD_ACCESSIBILITY -- hence the guard: without it there
// is no hub, and IhsApi::semantics is null rather than pointing at a table
// whose entry points do not exist.
const IhsSemanticsApi* semantics_api() noexcept;

}  // namespace ihs::semantics
#endif

#endif  // IHS_SHARED_SRC_IHS_INTERNAL_HPP_
