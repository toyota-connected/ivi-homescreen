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
 * SHELL-ONLY host seam for the OSGi bundle handshake. This is NOT part of the
 * plugin ABI (docs/PLUGIN_ABI.md) -- a consumer sees only ihs/ihs_osgi.h. It is
 * the private contract between libihs_shared, which owns the published
 * ihs_osgi_* surface, and the one in-process shell that owns the bundle
 * registry and the startup orchestrator.
 *
 *   bundle  --ihs_osgi_*-->  libihs_shared  --IhsOsgiHost-->  shell
 *
 * The split follows what each side actually knows, and here it is lopsided in a
 * way the semantics seam is not: libihs_shared owns nothing but the table
 * pointer. It validates arguments, then forwards. The shell owns all of it --
 * the port table, the ordering rule that makes registration commutative, the
 * Dart DL binding, and the orchestrator that decides what a critical bundle's
 * ACTIVE report releases.
 *
 * That is on purpose. Bundle lifecycle is a state machine the shell drives; a
 * copy of it on the far side of a C ABI would be a second source of truth for
 * which bundles exist, and the two would disagree exactly when it mattered.
 *
 * Threading: install the host once, at OSGi bring-up, before any bundle engine
 * is spawned. Every callback may be invoked from any thread -- each bundle
 * calls from its own isolate's thread -- so the implementation behind them
 * serializes for itself; BridgeRegistry already does.
 *
 * The shell must not call back into libihs_shared from inside a host callback.
 * The registry's lifecycle observer is dispatched outside the registry lock for
 * the same reason, and re-entering here would defeat that.
 */

#ifndef IHS_OSGI_HOST_H_
#define IHS_OSGI_HOST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ihs/ihs_export.h"
#include "ihs/ihs_osgi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Operations libihs_shared forwards to the shell. Each receives @user_data.
 *
 * Every entry returns an IhsOsgiStatus as int. A NULL entry means the
 * capability is absent: the forwarder reports IHS_OSGI_ERR_UNAVAILABLE rather
 * than pretending it succeeded.
 *
 *   register_framework
 *       Bind the Dart DL symbol table if it is not already bound, record the
 *       framework isolate's port, and post it to every bundle waiting for it.
 *       Write the number served to @out_served when non-null.
 *
 *   register_bundle
 *       Bind the Dart DL symbol table if needed, then record the bundle. The
 *       shell mints the handle; libihs_shared treats it as opaque and never
 *       dereferences it. Reject an unknown or duplicate symbolic name.
 *
 *       The handle must not be a cast of anything a caller could reconstruct --
 *       not the port, not a hash of the name, not an index into a vector.
 *       Reporting ACTIVE releases a critical bundle's startup wait, and any
 *       plugin in the process can reach these symbols; the handle is the only
 *       thing standing between that and a forged report. Treat it as a
 *       capability and mint it accordingly.
 *
 *   report_active / report_stopped
 *       The bundle's activator finished start() or stop(). The shell resolves
 *       the handle and routes to the orchestrator. An unrecognized handle is
 *       IHS_OSGI_ERR_REJECTED, not a crash: it is reachable from out-of-tree
 *       code and must be treated as untrusted input.
 *
 *   unregister
 *       Forget the bundle so a restart can re-register the same symbolic name,
 *       and invalidate the handle. Must tolerate a handle it does not know.
 */
typedef struct IhsOsgiHost {
  size_t struct_size; /* sizeof(IhsOsgiHost) */
  void* user_data;

  int (*register_framework)(void* user_data,
                            void* dart_api_dl_data,
                            int64_t port,
                            size_t* out_served);

  int (*register_bundle)(void* user_data,
                         void* dart_api_dl_data,
                         int64_t port,
                         const char* symbolic_name,
                         IhsOsgiBundle** out_bundle);

  int (*report_active)(void* user_data, IhsOsgiBundle* bundle);
  int (*report_stopped)(void* user_data, IhsOsgiBundle* bundle);
  int (*unregister)(void* user_data, IhsOsgiBundle* bundle);
} IhsOsgiHost;

/*
 * Install the host. Exactly one per process, at OSGi bring-up and before the
 * first bundle engine is spawned. Pass NULL to uninstall during teardown, after
 * which every ihs_osgi_* call reports IHS_OSGI_ERR_UNAVAILABLE rather than
 * calling into a shell that is going away.
 *
 * Unlike ihs_semantics_set_host, the struct is NOT copied: the table is read on
 * every forwarded call, and it must remain valid until uninstalled. A table
 * whose struct_size is smaller than this definition is refused outright -- the
 * struct only grows by appending, so a short one means an older shell against a
 * newer libihs_shared, and the trailing entries would be absent rather than
 * NULL. Leaving the forwarders on their safe defaults is degraded but
 * well-defined; calling through an absent entry is not.
 */
IHS_EXPORT void ihs_osgi_set_host(const IhsOsgiHost* host);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_OSGI_HOST_H_ */
