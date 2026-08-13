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
 * Tools an application declares for itself.
 *
 * The semantics tools are generic verbs -- tap this, set that -- derived from
 * what the framework already publishes, and they need no cooperation from the
 * application. This is the other kind: a typed, named operation the
 * application defines, with a JSON Schema for its arguments, so an agent can
 * call hvac_set_temp(zone, celsius) rather than assembling it out of taps on
 * whatever happens to be on screen.
 *
 * Why the arguments cannot ride a semantics action. A custom action is an id
 * and nothing else: the framework resolves a handler by it and passes no
 * parameters. So a typed call has to reach the application directly, which is
 * what `invoke` below is for -- there is no way to express this over the
 * semantics pipeline, and pretending otherwise would mean encoding arguments
 * into labels.
 *
 * Why this is a C entry point rather than a platform channel. libihs_shared is
 * already the supported binary interface to out-of-tree Dart FFI plugins (see
 * docs/PLUGIN_ABI.md), and registration is a control-plane operation that
 * happens on a route change rather than per frame -- so a wire format would be
 * new surface to own for no measurable gain. An application reaches this
 * through dart:ffi and hands back a NativeCallable for `invoke`.
 *
 * Lifecycle:
 *   1. ihs_mcp_app_tools_register with the tools and an invoke callback. The
 *      tools appear in tools/list at once, and clients are told the list
 *      changed.
 *   2. A client calls one. `invoke` fires with a call id and the arguments as
 *      the client sent them, unparsed -- the schema is the application's to
 *      enforce, since it wrote it.
 *   3. The application answers with ihs_mcp_app_tools_complete, from any
 *      thread. A call not completed within the timeout is reported to the
 *      client as a failure rather than left hanging.
 *   4. ihs_mcp_app_tools_unregister on teardown. Any call still outstanding is
 *      failed rather than abandoned, and no invoke fires afterwards.
 *
 * Registration is additive and optional: an application that declares nothing
 * still has the semantics tools, and the MCP surface works with no Dart-side
 * dependency at all.
 */

#ifndef IHS_MCP_APP_TOOLS_H_
#define IHS_MCP_APP_TOOLS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ihs/ihs_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An opaque registration. Valid until unregister returns. */
typedef struct IhsMcpAppTools IhsMcpAppTools;

/* One tool the application declares. */
typedef struct IhsMcpAppTool {
  /*
   * Unprefixed, as the application thinks of it ("set_temp"). The prefix from
   * the registration is applied before it is advertised, so an application
   * never has to repeat its own namespace.
   */
  const char* name;

  /* Shown to the agent choosing between tools. Not optional: a tool with no
   * description is one an agent has to guess at. */
  const char* description;

  /*
   * JSON Schema for the arguments, spliced into tools/list as-is. The
   * application owns both this and the parsing, so the two cannot disagree
   * about what the tool takes.
   */
  const char* input_schema_json;

  /*
   * Exactly one IHS_MCP_CAP_* bit, or 0 for IHS_MCP_CAP_INTERACT. A tool
   * outside the consumer's mask is dropped from the advertised list rather
   * than refused when called, as everywhere else.
   */
  uint64_t capability;
} IhsMcpAppTool;

/*
 * Invoked when a client calls one of the tools.
 *
 * `tool_name` is unprefixed, matching what was registered. `arguments_json` is
 * what the client sent, unparsed and not validated against the schema.
 *
 * Called on the thread serving the request, which is not the platform thread.
 * It must not block: the call is answered by ihs_mcp_app_tools_complete, which
 * may happen on any thread and at any later point, so an implementation is
 * free to hop to the UI thread and reply from there.
 */
typedef void (*IhsMcpAppToolInvoke)(void* user_data,
                                    uint64_t call_id,
                                    const char* tool_name,
                                    const char* arguments_json,
                                    size_t arguments_length);

typedef struct IhsMcpAppToolsDesc {
  size_t struct_size; /* sizeof(IhsMcpAppToolsDesc) */

  /*
   * Namespace for this application's tools, a bare identifier prefix
   * ("hvac_"). Checked for collision against every other provider, so an
   * application cannot shadow the built-in verbs or another application's.
   */
  const char* tool_prefix;

  /* Copied during registration; the caller may free them afterwards. */
  const IhsMcpAppTool* tools;
  size_t tool_count;

  IhsMcpAppToolInvoke invoke;
  void* user_data;

  /*
   * How long a call waits for the application before it is reported failed.
   * 0 selects a default. Bounded because the alternative is an agent blocked
   * on an application that will never answer, with no way to tell that from
   * one still working.
   */
  uint32_t call_timeout_ms;
} IhsMcpAppToolsDesc;

/*
 * Registers the tools. Returns IHS_MCP_OK, IHS_MCP_ERR_INVALID for a malformed
 * descriptor, or IHS_MCP_ERR_PREFIX_TAKEN when the prefix collides with one
 * already claimed -- kept distinct from a plain refusal, so a caller can tell
 * "another application owns this namespace" from "the host declined".
 *
 * Clients are notified that the tool list changed, so an agent that has
 * already listed tools learns about these without polling.
 */
IHS_EXPORT int ihs_mcp_app_tools_register(const IhsMcpAppToolsDesc* desc,
                                          IhsMcpAppTools** out_handle);

/*
 * Answers a call. `result_json` is returned to the client as the tool's
 * result; pass NULL for an empty one. `ok` false marks it as a tool error,
 * which is how a tool reports that it ran and failed as distinct from the tool
 * not existing.
 *
 * Returns IHS_MCP_ERR_NOT_FOUND for a call id that is unknown or has already
 * been answered -- including one that timed out, so a late reply is reported
 * rather than silently discarded.
 */
IHS_EXPORT int ihs_mcp_app_tools_complete(uint64_t call_id,
                                          bool ok,
                                          const char* result_json,
                                          size_t result_length);

/*
 * Unregisters. Any call still outstanding is failed before this returns, and
 * no invoke fires afterwards, so the callback and its user_data are safe to
 * tear down once this has returned. Passing NULL is a no-op.
 */
IHS_EXPORT void ihs_mcp_app_tools_unregister(IhsMcpAppTools* handle);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_MCP_APP_TOOLS_H_ */
