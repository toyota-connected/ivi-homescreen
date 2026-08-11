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
 * Registers the semantics provider. Returns an IhsMcpStatus: IHS_MCP_OK, or
 * IHS_MCP_ERR_PREFIX_TAKEN if something already claims `ui_` / `ui://`.
 *
 * Idempotent -- starting an already-started provider succeeds without
 * registering twice, so a caller that cannot easily tell whether bring-up
 * already ran does not have to.
 */
IHS_EXPORT int ihs_mcp_semantics_provider_start(void);

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
