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
 * SHELL-ONLY routing surface for the MCP provider registry. This is NOT part
 * of the plugin ABI (docs/PLUGIN_ABI.md) -- a provider sees only
 * ihs/ihs_mcp_provider.h.
 *
 *   provider --ihs_mcp_provider_*--> registry <--ihs_mcp_host_*-- MCP server
 *
 * The registry sits between them so neither has to know about the other. It
 * owns namespacing, capability masking, and the lifetime rules; the server
 * above it owns the socket and the protocol framing, and knows nothing about
 * any particular capability.
 *
 * Everything here is deliberately transport-free: it answers "what tools
 * exist" and "run this one" without reference to a session, a socket, or a
 * JSON-RPC envelope. That keeps the whole routing layer testable without a
 * server, and keeps the server's job to translating MCP messages into these
 * calls.
 *
 * Built only when BUILD_MCP is on; see ihs/ihs_mcp_provider.h.
 *
 * Threading: callable from any thread; the registry serializes internally.
 * Provider callbacks are invoked on the calling thread, which for the real
 * server is the host thread -- matching what ihs_mcp_provider.h promises
 * providers.
 */

#ifndef IHS_MCP_HOST_H_
#define IHS_MCP_HOST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ihs/ihs_export.h"
#include "ihs/ihs_mcp_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One tool as clients see it: the provider's name with the provider's prefix
 * already applied, so `name` is what a call_tool request will carry.
 *
 * `provider` is the owning provider's name, for logs and for attribution when
 * something has to be traced back to who offered it.
 */
typedef struct IhsMcpHostTool {
  const char* name; /* prefixed */
  const char* description;
  const char* input_schema_json;
  const char* provider;
  uint64_t capability;
} IhsMcpHostTool;

/* One resource as clients see it. */
typedef struct IhsMcpHostResource {
  const char* uri;
  const char* name;
  const char* description;
  const char* mime_type;
  const char* provider;
} IhsMcpHostResource;

/*
 * Visits every advertised tool across all providers, in registration order.
 *
 * Tools whose capability is outside their provider's mask are not visited:
 * masking happens here, at the point of advertisement, so a masked-out tool
 * never reaches a client's tool list and cannot be discovered by name.
 *
 * `fn` must not call back into the registry -- it is invoked with the
 * registry's own lock held, and the pointers it receives are valid only for
 * the duration of the call.
 */
typedef void (*IhsMcpToolVisitor)(const IhsMcpHostTool* tool, void* user_data);
IHS_EXPORT int ihs_mcp_host_for_each_tool(IhsMcpToolVisitor fn,
                                          void* user_data);

/* The same, for resources. */
typedef void (*IhsMcpResourceVisitor)(const IhsMcpHostResource* resource,
                                      void* user_data);
IHS_EXPORT int ihs_mcp_host_for_each_resource(IhsMcpResourceVisitor fn,
                                              void* user_data);

/*
 * Routes a tool call by prefix to its provider and strips the prefix before
 * handing the name on, so a provider only ever sees its own vocabulary.
 *
 * Returns IHS_MCP_ERR_NOT_FOUND when no provider claims the prefix or the
 * provider does not offer the tool, and IHS_MCP_ERR_CAPABILITY_DENIED when the
 * tool exists but is masked out. The two are distinct on purpose: a masked
 * tool is invisible in the listing, so a client asking for it by name has
 * either been told about it elsewhere or is probing, and conflating that with
 * an ordinary typo would hide it.
 *
 * `arguments_json` may be NULL, which is passed to the provider as "{}".
 * On IHS_MCP_OK the caller owns the payload and must release it with
 * ihs_mcp_host_release_payload.
 */
IHS_EXPORT int ihs_mcp_host_call_tool(const char* name,
                                      const char* arguments_json,
                                      size_t arguments_length,
                                      IhsMcpPayload* out_result);

/*
 * Reads a resource, routed by URI scheme. Returns IHS_MCP_ERR_NOT_FOUND when
 * no provider claims the scheme or the provider does not serve the URI. On
 * IHS_MCP_OK the caller owns the payload.
 */
IHS_EXPORT int ihs_mcp_host_read_resource(const char* uri,
                                          IhsMcpPayload* out_content);

/*
 * Notes client interest in a URI, routed by scheme. Providers that left
 * `subscribe` NULL are a no-op success: the host tracks subscriptions itself
 * and the callback exists only so a provider can start or stop work.
 */
IHS_EXPORT int ihs_mcp_host_subscribe(const char* uri, bool subscribed);

/*
 * What a provider changing underneath the server looks like from above.
 *
 * `kind` says which notification the server should emit; `uri` names the
 * resource for IHS_MCP_NOTIFY_RESOURCE_UPDATED and is "" otherwise.
 *
 * Invoked on the registry's watcher thread, never with the registry lock held,
 * so a sink may call back in -- reading the tool list on a change is the
 * obvious thing to want and would otherwise deadlock.
 */
typedef enum IhsMcpNotification {
  /* A provider's tool list changed; re-read it and tell clients. */
  IHS_MCP_NOTIFY_TOOLS_CHANGED = 0,
  /* A resource changed; tell clients subscribed to `uri`. */
  IHS_MCP_NOTIFY_RESOURCE_UPDATED = 1
} IhsMcpNotification;

typedef void (*IhsMcpNotificationSink)(IhsMcpNotification kind,
                                       const char* uri,
                                       void* user_data);

/*
 * Installs the sink the registry pushes provider changes to, and starts
 * watching every registered provider's notify_fd. Pass NULL to uninstall and
 * stop watching; the call does not return until the watcher is joined, so a
 * caller may then tear down whatever the sink wrote to.
 *
 * One sink per process -- the server is the only thing that can deliver a
 * notification, and a second one would mean two servers on one registry.
 * Returns IHS_MCP_OK, or IHS_MCP_ERR_REFUSED if a sink is already installed.
 *
 * Without a sink the registry never watches an fd, so a build with no server
 * pays nothing for a provider that signals into one.
 */
IHS_EXPORT int ihs_mcp_host_set_notification_sink(IhsMcpNotificationSink sink,
                                                  void* user_data);

/*
 * Releases a payload a provider produced. Calls the provider's own release
 * hook; safe on a zeroed payload, so a caller can release unconditionally on
 * an error path without checking whether anything was produced.
 */
IHS_EXPORT void ihs_mcp_host_release_payload(IhsMcpPayload* payload);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_MCP_HOST_H_ */
