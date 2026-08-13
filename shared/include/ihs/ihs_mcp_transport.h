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
 * The MCP transport: a Unix domain socket carrying JSON-RPC 2.0 over HTTP,
 * which is what the MCP Streamable HTTP transport is once the network is taken
 * out of it. Everything it serves comes from ihs_mcp_registry.h; it adds
 * framing and a socket, and knows nothing about tools or the semantics tree.
 *
 * Unix domain only, deliberately. A TCP listener would make the UI drivable
 * from off-box, which is a product-security decision rather than a transport
 * one, so there is no address family to configure and no token to get wrong.
 * A filesystem socket puts the decision where the operating system can enforce
 * it: the socket is created 0600, and every connection's peer credentials are
 * checked against the shell's own uid before a byte is read.
 */

#ifndef IHS_MCP_TRANSPORT_H_
#define IHS_MCP_TRANSPORT_H_

#include <stdbool.h>
#include <stddef.h>

#include "ihs/ihs_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IhsMcpTransportConfig {
  size_t struct_size; /* sizeof(IhsMcpTransportConfig) */

  /*
   * Where to listen. NULL or "" selects
   * $XDG_RUNTIME_DIR/ivi-homescreen/mcp.sock, falling back to
   * /run/user/<uid>/ivi-homescreen/mcp.sock. The parent directory is created
   * 0700 if missing.
   *
   * An existing socket at the path is replaced only when nothing is listening
   * on it, so a second shell cannot silently steal the first one's endpoint.
   */
  const char* socket_path;

  /*
   * Largest request body accepted, in bytes. 0 selects 1 MiB. A request that
   * declares more is refused before anything is read, so an oversized
   * Content-Length costs nothing to reject.
   */
  size_t max_request_bytes;
} IhsMcpTransportConfig;

/*
 * Binds the socket and serves on a thread of its own. Returns IHS_MCP_OK, or
 * IHS_MCP_ERR_INVALID for a malformed config, IHS_MCP_ERR_REFUSED when the
 * path is already being served by a live listener, and
 * IHS_MCP_ERR_UNAVAILABLE when the socket cannot be created or bound.
 *
 * Idempotent: starting an already-started transport succeeds and keeps the
 * original socket, so a caller that cannot easily tell whether bring-up
 * already ran does not have to.
 */
IHS_EXPORT int ihs_mcp_transport_start(const IhsMcpTransportConfig* config);

/*
 * Stops serving, closes every connection, joins the thread and unlinks the
 * socket. Safe to call when not started. After it returns no request is in
 * flight, so a caller may tear down the host it routes to.
 */
IHS_EXPORT void ihs_mcp_transport_stop(void);

/* Whether the transport is currently serving. Diagnostics. */
IHS_EXPORT bool ihs_mcp_transport_running(void);

/*
 * The path actually bound, or "" when not started. Callers log this rather
 * than recomputing the default. Valid until the next stop.
 */
IHS_EXPORT const char* ihs_mcp_transport_socket_path(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_MCP_TRANSPORT_H_ */
