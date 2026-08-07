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
 * Provider surface for the in-process MCP host.
 *
 * The host owns one server: the socket, the session, authentication, and the
 * MCP protocol framing. It does not own any particular capability. Anything
 * that wants to expose tools or resources registers here as a provider, and
 * the host multiplexes them onto that single server.
 *
 * The alternative -- a server per capability -- was rejected: each one would
 * grow its own socket, its own authentication story, and its own security
 * review, and a client would need configuring for each. One host means one
 * attack surface and one client configuration, which matters more here than
 * the isolation separate servers would buy.
 *
 * Namespacing is by prefix and is checked at registration, not at call time. A
 * provider claims a tool prefix ("ui_") and a resource scheme ("ui://"), and
 * the host refuses a registration whose prefixes collide with one already
 * present. Two providers developed independently therefore cannot silently
 * shadow each other's tools; the second one fails to register and says why.
 *
 * Lifecycle of one provider:
 *   1. Fill an IhsMcpProviderDesc and call ihs_mcp_provider_register. The host
 *      validates the prefixes, applies the capability mask to the declared
 *      tools, and emits notifications/tools/list_changed to connected clients.
 *   2. Serve list_tools / call_tool / list_resources / read_resource as the
 *      host calls them. All are invoked on the host thread.
 *   3. When a resource changes, write to notify_fd. The host coalesces and
 *      emits notifications/resources/updated for the subscribed URIs.
 *   4. ihs_mcp_provider_unregister removes the tools and resources and emits
 *      tools/list_changed again. It does not return until no call into this
 *      provider is in flight.
 *
 * Threading: register/unregister may be called from any thread. Every callback
 * in IhsMcpProviderDesc is invoked on the host thread and must not block on
 * it -- a provider whose work belongs on another thread owns that hop itself.
 * The host deliberately does not thread-hop on a provider's behalf, because it
 * has no way to know which thread a given provider's state belongs to.
 *
 * This surface has no dependency on the semantics hub or on any Flutter
 * header. A provider may be an out-of-tree FFI plugin; see docs/PLUGIN_ABI.md
 * for the boundary rules. Registration lives in libihs_shared for that reason
 * and no other: a dlopen'd plugin can only bind to a shared object, so a
 * provider implemented in one has to resolve these symbols from there.
 *
 * Built only when BUILD_MCP is on, which is off by default. This is remote
 * control of the UI, so an image opts in at build time and again at runtime
 * through config before any of it is reachable. With BUILD_MCP off the
 * implementation is not compiled, no ihs_mcp_* symbol is exported, and this
 * header is not installed -- so an out-of-tree consumer finds out at the
 * include rather than at the link.
 */

#ifndef IHS_MCP_PROVIDER_H_
#define IHS_MCP_PROVIDER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ihs/ihs_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A registered provider. Valid until ihs_mcp_provider_unregister returns. */
typedef struct IhsMcpProvider IhsMcpProvider;

/* Status codes. Negative values are errors. */
typedef enum IhsMcpStatus {
  IHS_MCP_OK = 0,
  /* A required argument was null, or a struct_size was not recognized. */
  IHS_MCP_ERR_INVALID = -1,
  /* The tool or resource prefix is already claimed by another provider. */
  IHS_MCP_ERR_PREFIX_TAKEN = -2,
  /* The tool exists but its capability is not in the provider's mask, or the
   * host is refusing the class outright for this build. */
  IHS_MCP_ERR_CAPABILITY_DENIED = -3,
  /* No such tool or resource under this provider. */
  IHS_MCP_ERR_NOT_FOUND = -4,
  /* The provider understood the request and declined it. Distinct from a
   * malformed one so a client can tell "I asked wrongly" from "I asked for
   * something you will not do". */
  IHS_MCP_ERR_REFUSED = -5,
  /* No host: MCP was never started, or is shutting down. */
  IHS_MCP_ERR_UNAVAILABLE = -6
} IhsMcpStatus;

/*
 * Tool capability classes, for IhsMcpToolDesc::capability and the provider's
 * capability_mask.
 *
 * The split exists so a build can ship a provider with its dangerous tools
 * compiled in but unreachable, rather than maintaining two builds of the
 * provider. The host applies the mask when the tool list is assembled, so a
 * masked-out tool is not merely rejected on call -- it is never advertised,
 * and a client cannot discover that it exists.
 *
 * This is a different question from the semantics hub's action_allow_mask,
 * which governs which semantics actions a hub consumer may dispatch. That one
 * is about arbitration between actors sharing the UI; this one is about which
 * tool classes a build exposes at all. Both are enforced, at different layers.
 */
/* Reads state and has no side effect the user could observe. */
#define IHS_MCP_CAP_INSPECT UINT64_C(0x0000000000000001)
/* Drives the UI the way a user would: activates, types, scrolls. */
#define IHS_MCP_CAP_INTERACT UINT64_C(0x0000000000000002)
/* Reaches past the UI into application or engine state directly. Off in
 * production images unless a product decision says otherwise. */
#define IHS_MCP_CAP_PRIVILEGED UINT64_C(0x0000000000000004)

/* Everything this ABI version defines. Spelled as the OR of the named bits so
 * it cannot drift when a class is added. */
#define IHS_MCP_CAP_ALL \
  (IHS_MCP_CAP_INSPECT | IHS_MCP_CAP_INTERACT | IHS_MCP_CAP_PRIVILEGED)

/* The conservative default: look, do not touch. */
#define IHS_MCP_CAP_READ_ONLY IHS_MCP_CAP_INSPECT

/*
 * One tool a provider offers.
 *
 * `name` is unprefixed -- the host prepends the provider's tool_prefix -- so a
 * provider never has to repeat its own namespace and cannot accidentally
 * advertise outside it.
 *
 * `input_schema_json` is a JSON Schema object, as MCP requires. It is passed
 * as text rather than modeled in C on purpose: MCP is JSON on the wire, so a
 * parallel C object model would exist only to be serialized back to the JSON
 * the provider already had. The host does not interpret it beyond checking it
 * parses; a provider is responsible for its own argument validation, because
 * the host cannot know what a given tool considers valid.
 */
typedef struct IhsMcpToolDesc {
  const char* name;
  const char* description;
  const char* input_schema_json;
  uint64_t capability; /* exactly one IHS_MCP_CAP_* bit */
} IhsMcpToolDesc;

/* One resource a provider offers. `uri` is complete, including the provider's
 * scheme prefix, since a resource URI is meaningful to clients as a whole. */
typedef struct IhsMcpResourceDesc {
  const char* uri;
  const char* name;
  const char* description;
  const char* mime_type; /* "application/json", "text/plain", ... */
} IhsMcpResourceDesc;

/*
 * A UTF-8 payload a provider hands back.
 *
 * The provider owns the bytes. When the host is finished it calls `release`
 * with `release_ctx`, which may be NULL for a static or otherwise
 * caller-managed buffer. Ownership is explicit rather than "valid until the
 * next call" because a host that pipelines requests would break the latter
 * silently and only under load.
 */
typedef struct IhsMcpPayload {
  size_t struct_size; /* sizeof(IhsMcpPayload) */
  const char* data;
  size_t length;
  void (*release)(void* release_ctx);
  void* release_ctx;
} IhsMcpPayload;

/*
 * What a provider implements. Every callback receives the desc's `user_data`
 * and runs on the host thread.
 *
 * A NULL callback means the provider does not offer that capability: no tools,
 * no resources, or no subscriptions. The host reports that to clients as an
 * empty list rather than an error, so a provider that only serves resources
 * needs no tool stubs.
 */
typedef struct IhsMcpProviderCallbacks {
  /*
   * Returns the provider's current tools. The array must stay valid until the
   * next list_tools on this provider or until unregister returns, whichever
   * comes first -- the host copies what it needs before returning to the
   * client, so the window is short and does not span a call_tool.
   *
   * A provider whose tool set changes writes to notify_fd; the host re-lists
   * and emits tools/list_changed rather than requiring the provider to
   * describe the delta.
   */
  int (*list_tools)(void* user_data,
                    const IhsMcpToolDesc** out_tools,
                    size_t* out_count);

  /*
   * Invokes a tool. `name` is unprefixed, matching what list_tools returned.
   * `arguments_json` is the client's arguments as a JSON object, never NULL --
   * an absent argument set arrives as "{}" so a provider needs no null check.
   *
   * On IHS_MCP_OK, fill `out_result` with the tool's content. On any error,
   * leave it untouched and return a status; the host turns that into an MCP
   * tool error carrying a message it derives from the status.
   */
  int (*call_tool)(void* user_data,
                   const char* name,
                   const char* arguments_json,
                   size_t arguments_length,
                   IhsMcpPayload* out_result);

  /* Same contract as list_tools, for resources. */
  int (*list_resources)(void* user_data,
                        const IhsMcpResourceDesc** out_resources,
                        size_t* out_count);

  /* Reads one resource by its full URI. */
  int (*read_resource)(void* user_data,
                       const char* uri,
                       IhsMcpPayload* out_content);

  /*
   * Notes a client's interest in a URI. The host tracks subscriptions itself
   * and only calls this so a provider can start or stop work it would
   * otherwise not do -- a provider that always has its data current may leave
   * this NULL and lose nothing.
   */
  int (*subscribe)(void* user_data, const char* uri, bool subscribed);
} IhsMcpProviderCallbacks;

/*
 * Provider registration.
 *
 * notify_fd is owned by the caller and must outlive the registration. The
 * provider writes to it when its tool list or any resource has changed; the
 * host coalesces and re-reads. Pass -1 for a provider whose surface never
 * changes. As with the semantics hub, this is an fd rather than a callback so
 * that one provider cannot delay another's notifications by blocking.
 */
typedef struct IhsMcpProviderDesc {
  size_t struct_size; /* sizeof(IhsMcpProviderDesc) */

  /* Stable and human-meaningful; appears in host logs and in the error when a
   * prefix collides. Not optional. */
  const char* name;

  /* Claimed namespaces, both checked for collision at registration.
   * `tool_prefix` is a bare identifier prefix ("ui_"); `resource_scheme` is a
   * URI scheme without the separator ("ui"), which the host renders as
   * "ui://". */
  const char* tool_prefix;
  const char* resource_scheme;

  /* Bitwise OR of IHS_MCP_CAP_*. A tool whose capability is outside this mask
   * is dropped from the advertised list, not merely refused when called. */
  uint64_t capability_mask;

  IhsMcpProviderCallbacks callbacks;
  void* user_data;

  int notify_fd;
} IhsMcpProviderDesc;

/*
 * Register. On success *out_provider receives a handle.
 *
 * Fails with IHS_MCP_ERR_PREFIX_TAKEN if either namespace is already claimed;
 * the registration is rejected whole rather than partially applied, so a
 * provider is never half-present.
 *
 * Registering when no host is running is not an error: the provider is held
 * and becomes live when the host starts. Otherwise every provider would have
 * to care about the order it loads in relative to the server.
 */
IHS_EXPORT int ihs_mcp_provider_register(const IhsMcpProviderDesc* desc,
                                         IhsMcpProvider** out_provider);

/*
 * Unregister. Does not return until no callback into this provider is in
 * flight, so a provider may free its state as soon as this returns. Emits
 * notifications/tools/list_changed to connected clients.
 */
IHS_EXPORT void ihs_mcp_provider_unregister(IhsMcpProvider* provider);

/* Number of providers currently registered. Diagnostics; the host does not
 * gate anything on this. */
IHS_EXPORT size_t ihs_mcp_provider_count(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_MCP_PROVIDER_H_ */
