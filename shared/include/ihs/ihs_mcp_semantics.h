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
 * SHELL-ONLY lifecycle for the semantics MCP provider -- the surface that lets
 * a client inspect the UI and drive it. Not part of the plugin ABI.
 *
 * The provider is a hub consumer and an MCP provider at once: it reads
 * published snapshots to answer queries, and sends actions back through
 * ihs_semantics_dispatch. It claims the `ui_` tool prefix and the `ui://`
 * resource scheme.
 *
 * Starting it registers with both the hub and the MCP registry. Registering
 * with the hub is what asks the engine for semantics, so nothing is produced
 * until something actually wants it.
 *
 * The tools it offers are the ones the framework can act on directly, so an
 * accessibility-correct app is drivable with no extra work: a tap dispatch
 * invokes the same handler the widget registered for a real tap.
 *
 * Threading: start/stop from one thread, typically at engine bring-up and
 * teardown. Everything else is driven by the MCP host.
 */

#ifndef IHS_MCP_SEMANTICS_H_
#define IHS_MCP_SEMANTICS_H_

#include <stdbool.h>
#include <stddef.h>

#include "ihs/ihs_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What this provider is permitted to offer (ABI 1.3).
 *
 * The compiled default is already correct -- everything the tree offers except
 * the accessibility-focus actions, which an agent must never reach. This
 * narrows it for an image that wants less, and cannot widen it past the
 * default: a configuration file that has to be present for the shell to be
 * safe is a configuration file whose absence is a vulnerability.
 */
typedef struct IhsMcpSemanticsConfig {
  size_t struct_size; /* sizeof(IhsMcpSemanticsConfig) */

  /*
   * Whether `allowed_tools` applies at all. False ignores it and offers the
   * compiled default.
   *
   * This is a separate field rather than a NULL check on the pointer, because
   * the most restrictive policy is an *empty* list and the natural way to
   * produce one -- `.data()` on an empty container -- yields NULL. Reading
   * presence off the pointer would have made "offer nothing" and "offer
   * everything" the same call, which is the worst direction for that mistake
   * to run in.
   */
  bool narrow_tools;

  /*
   * Tool names this provider may offer, unprefixed and spelled exactly as the
   * tools are named -- "tap", "set_text", "tap_at". Read only when
   * `narrow_tools` is true, and may be NULL when the count is zero.
   *
   * An empty list is a read-only surface: an agent can still read the tree and
   * query it, because reading is what the tree is for and refusing it would
   * leave the surface with no purpose, but it can change nothing. "snapshot"
   * and "query" are therefore always offered and need not be listed.
   *
   * An unrecognised name is an error rather than a silent omission. Mistaking
   * a name in a policy file should not quietly grant less than intended, and
   * least of all should it quietly grant more.
   */
  const char* const* allowed_tools;
  size_t allowed_tool_count;
} IhsMcpSemanticsConfig;

/*
 * Registers the semantics provider with the compiled default policy.
 * Equivalent to ihs_mcp_semantics_provider_start_with(NULL).
 *
 * Returns an IhsMcpStatus: IHS_MCP_OK, or IHS_MCP_ERR_PREFIX_TAKEN if
 * something already claims `ui_` / `ui://`.
 *
 * Idempotent -- starting an already-started provider succeeds without
 * registering twice, so a caller that cannot easily tell whether bring-up
 * already ran does not have to.
 */
IHS_EXPORT int ihs_mcp_semantics_provider_start(void);

/*
 * As above, with a policy. NULL config selects the compiled default.
 *
 * Returns IHS_MCP_ERR_INVALID for a malformed config or an unrecognised tool
 * name, in which case nothing is registered -- a policy that could not be
 * applied must not leave a surface running under some other one.
 *
 * Idempotent in the same way as the no-argument form, which means the policy
 * of the *first* successful start is the one in force. Restarting with a
 * different policy requires a stop.
 */
IHS_EXPORT int ihs_mcp_semantics_provider_start_with(
    const IhsMcpSemanticsConfig* config);

/*
 * Unregisters from both the MCP registry and the hub. Safe to call when not
 * started. After it returns, no callback into the provider is in flight and
 * its hub registration is released -- which, if it was the last consumer, is
 * what turns engine semantics back off.
 */
IHS_EXPORT void ihs_mcp_semantics_provider_stop(void);

/* Whether the provider is currently registered. Diagnostics. */
IHS_EXPORT bool ihs_mcp_semantics_provider_running(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_MCP_SEMANTICS_H_ */
