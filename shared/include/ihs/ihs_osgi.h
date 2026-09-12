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
 * The OSGi bundle handshake, re-exported from libihs_shared.so.
 *
 * A bundle is a Flutter engine running Dart code, and it has to tell the shell
 * two things native code cannot obtain for itself: the address of Dart's
 * NativeApi.initializeApiDLData, so the shell can bind Dart_PostCObject_DL, and
 * a receive port to post to. It later reports that its activator finished,
 * which is what releases a critical bundle's startup wait.
 *
 * That already works over the dev.osgi/bridge MethodChannel. This surface
 * exists because the channel drags Flutter into the dependency graph of every
 * bundle, including headless ones with no views: a bundle that wants only to
 * decode CAN frames must still initialize a UI binding to send three integers,
 * and its package must resolve against the Flutter SDK to do it. Over FFI the
 * same handshake needs nothing but dart:ffi, so a service bundle is an ordinary
 * Dart package that `dart test` can exercise.
 *
 * The second reason is the platform thread. A MethodChannel reply is marshalled
 * through the platform task runner, which is at its busiest during exactly the
 * window an ACTIVE report has to cross -- first frame, pipeline warm-up -- and
 * the critical bundle's startup deadline is running the whole time. These calls
 * are synchronous and touch no task runner.
 *
 * WHY THESE ARE FORWARDERS, AND WHERE THE STATE IS
 *
 * The registry that backs this lives in the shell (shell/osgi/bridge_registry),
 * not in this library, and these functions only forward to it over a proc table
 * the shell installs -- the same shape as the FlutterDesktop* re-exports (see
 * flutter_desktop_bridge.h) and for the same reason: the implementation is in
 * the executable, which is not a shared object a dlopen'd plugin can bind to.
 *
 * The state stays shell-side deliberately, and this is the one design decision
 * here worth defending. Bundle lifecycle is not self-contained bookkeeping the
 * way the MCP provider registry is: the startup orchestrator owns which bundles
 * are critical, what their deadlines are, and which thread the reactor waits
 * on. Moving the registry into this library would put a C ABI through the
 * middle of that state machine, would drag the Dart DL headers into a library
 * whose whole value is that it builds standalone with almost no dependencies,
 * and would strand the registry's existing unit tests, which fake the two Dart
 * DL entry points to test the ordering hazard without a VM.
 *
 * So: this library holds no OSGi state at all. Every function below reads one
 * atomic pointer and calls through it.
 *
 * SAFE DEFAULTS
 *
 * Until the shell installs its table, every call returns
 * IHS_OSGI_ERR_UNAVAILABLE. That is the honest answer in a build with
 * ENABLE_OSGI off, or in a process that is not ivi-homescreen at all, and it is
 * the state a plugin should expect to handle: an OSGi bundle running under a
 * shell that does not do OSGi is a configuration mistake, not a crash.
 *
 * THREADING
 *
 * Every function may be called from any thread. Bundles call from their own
 * isolate's thread, and there is one per engine. The registry serializes
 * internally.
 *
 * A bundle must not call back into this surface from inside a shell callback;
 * there are none in this direction today, and that is deliberate.
 *
 * Built only when ENABLE_OSGI is on. With it off, nothing here is compiled, no
 * ihs_osgi_* symbol is exported, and this header is not installed -- so an
 * out-of-tree consumer finds out at the include rather than at the link.
 */

#ifndef IHS_OSGI_H_
#define IHS_OSGI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ihs/ihs_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A registered bundle. Valid until ihs_osgi_unregister returns.
 *
 * This is a capability, not a convenience. Reporting ACTIVE releases the
 * shell's critical-bundle wait, so a caller that can do it by naming a string
 * can release the wait for a bundle that is not ready -- and because that path
 * is meant to unblock startup, nothing downstream would look wrong. Every
 * ihs_* symbol is reachable by anything in the process that dlopens this
 * library, so a name would be no barrier at all.
 *
 * Requiring the handle means forging a report costs a completed registration
 * under a symbolic name the shell's config declares, which is a materially
 * higher bar. The handle is minted by the registry, is not derived from the
 * name or the port, and is never sent to Dart as anything but an opaque
 * integer.
 */
typedef struct IhsOsgiBundle IhsOsgiBundle;

/*
 * Status codes. Negative values are errors. Returned as int so the enum's
 * underlying width is not part of the ABI.
 */
typedef enum IhsOsgiStatus {
  IHS_OSGI_OK = 0,
  /* A required argument was null, a struct_size was not recognized, or a port
   * was zero. */
  IHS_OSGI_ERR_INVALID = -1,
  /* Dart_InitializeApiDL failed: the shell's vendored Dart DL headers do not
   * match the running VM. Nothing else on this surface will work. */
  IHS_OSGI_ERR_DART_API = -2,
  /* The shell declined. For a bundle the usual cause is a symbolic_name
   * matching no [[osgi.bundles]] entry, or one already registered -- a config
   * mismatch, surfaced early rather than left to run unmanaged. */
  IHS_OSGI_ERR_REJECTED = -3,
  /* No OSGi host: built without ENABLE_OSGI, not an ivi-homescreen process, or
   * the shell is shutting down. */
  IHS_OSGI_ERR_UNAVAILABLE = -4
} IhsOsgiStatus;

/*
 * Common to both registrations.
 *
 * @dart_api_dl_data is the address of Dart's NativeApi.initializeApiDLData, as
 * an integer widened to a pointer. Every caller supplies it because any isolate
 * may be the first to arrive; the shell makes all but the first a no-op. It is
 * a VM-global and identical for every isolate in the process, since all the
 * engines here share one VM.
 *
 * @port is the caller's SendPort.nativePort. Zero is rejected: a closed or
 * never-opened port would look like a successful handshake and then silently
 * deliver nothing.
 */
typedef struct IhsOsgiPeerInfo {
  size_t struct_size; /* sizeof(IhsOsgiPeerInfo) */
  void* dart_api_dl_data;
  int64_t port;
} IhsOsgiPeerInfo;

/*
 * Register the framework isolate and hand its port to every bundle already
 * waiting for it.
 *
 * Exactly one framework per process. @out_served, when non-null, receives the
 * number of bundles that were waiting and have now been served; it is a
 * diagnostic, and zero is normal when the framework wins the race.
 *
 * Registration is commutative with ihs_osgi_register_bundle on purpose: the
 * framework isolate and the bundle engines start concurrently, so either order
 * happens, and whichever arrives second triggers delivery. Nothing here depends
 * on startup order.
 */
IHS_EXPORT int ihs_osgi_register_framework(const IhsOsgiPeerInfo* info,
                                           size_t* out_served);

/*
 * A bundle's registration.
 *
 * @symbolic_name must match an [[osgi.bundles]] entry in the shell's config.
 * The shell refuses a name it does not know rather than accepting it, because
 * the likeliest cause is a config mismatch and that should be loud.
 */
typedef struct IhsOsgiBundleInfo {
  size_t struct_size; /* sizeof(IhsOsgiBundleInfo) */
  IhsOsgiPeerInfo peer;
  const char* symbolic_name; /* copied; need not outlive the call */
} IhsOsgiBundleInfo;

/*
 * Register a bundle. On IHS_OSGI_OK, *out_bundle holds the handle every later
 * call needs.
 *
 * The framework port is not returned here, and not because it would be
 * inconvenient: it may not exist yet. It is posted to @info->peer.port when the
 * framework registers, which may be before or after this call. A bundle waits
 * on its receive port either way, and a bundle with no interest in other
 * bundles never waits at all.
 */
IHS_EXPORT int ihs_osgi_register_bundle(const IhsOsgiBundleInfo* info,
                                        IhsOsgiBundle** out_bundle);

/*
 * Report that the bundle's activator finished start().
 *
 * This is what ACTIVE means in the OSGi lifecycle. Not that the engine is up,
 * which the shell already knows, and not that a frame was presented, which says
 * nothing about whether the bundle's own code is ready. A critical bundle's
 * startup wait is released by this and nothing else, so sending it before
 * start-up work is genuinely finished defeats the guarantee the shell is
 * holding the reactor for.
 */
IHS_EXPORT int ihs_osgi_report_active(IhsOsgiBundle* bundle);

/*
 * Report that the bundle's activator finished stop().
 *
 * The bundle stays registered: STOPPING returns to RESOLVED, from which it can
 * start again. Call ihs_osgi_unregister only when the bundle is going away.
 */
IHS_EXPORT int ihs_osgi_report_stopped(IhsOsgiBundle* bundle);

/*
 * Release the registration, so a restart can re-register the same symbolic
 * name. The handle is invalid once this returns.
 *
 * Idempotent against a null handle, so a teardown path that runs after a failed
 * registration needs no special case -- teardown is usually running because
 * something else already failed, and a second failure there buries the first.
 */
IHS_EXPORT int ihs_osgi_unregister(IhsOsgiBundle* bundle);

/*
 * Whether an OSGi host is installed. Lets a bundle choose a transport at
 * start-up instead of provoking an error to find out.
 *
 * Advisory only: the host can go away between this call and the next, and every
 * other function reports IHS_OSGI_ERR_UNAVAILABLE for itself.
 */
IHS_EXPORT bool ihs_osgi_available(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_OSGI_H_ */
