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
 * Fuzzes the MCP transport by speaking arbitrary bytes to its socket.
 *
 * The input is the whole client side of one connection -- request line,
 * headers and body, unmodified. Driving it from outside rather than calling
 * the parsers directly is deliberate: the header block, the Content-Length
 * accounting, the JSON-RPC envelope and the routing onto a provider all see
 * the same bytes a client would send, in the same order, and a bug that
 * depends on how they interact is reachable. Calling an internal parser with
 * a tidy string would test the parser and not the arrangement.
 *
 * The reason this surface is worth the machinery: peer credentials are the
 * whole local trust model, but off-box access terminates TLS in front of the
 * socket (docs/mcp-remote-access.md), and past that terminator every byte
 * here came from somewhere else. The parser runs before any of it is
 * understood.
 *
 * A provider is registered so tools/list, tools/call, resources/read and the
 * subscription paths route somewhere instead of bottoming out in "no such
 * tool" -- which would leave most of the dispatcher unreached.
 */

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ihs/ihs_mcp_provider.h"
#include "ihs/ihs_mcp_transport.h"

namespace {

// Long enough that a slow reply is still collected, short enough that a
// wedged server shows up as a throughput collapse rather than a hang.
constexpr int kReplyTimeoutMs = 250;

std::string* g_socket_path = nullptr;

// A provider with one tool and one resource. It answers rather than
// declining, so a call that parses reaches the reply-building path too.
struct EchoProvider {
  static int ListTools(void*, const IhsMcpToolDesc** out, size_t* count) {
    static const IhsMcpToolDesc kTools[] = {
        {"echo", "Returns its arguments.",
         R"({"type":"object","properties":{"text":{"type":"string"}}})",
         IHS_MCP_CAP_INSPECT},
        {"act", "Pretends to do something.", R"({"type":"object"})",
         IHS_MCP_CAP_INTERACT},
    };
    *out = kTools;
    *count = sizeof(kTools) / sizeof(kTools[0]);
    return IHS_MCP_OK;
  }

  static int ListResources(void*,
                           const IhsMcpResourceDesc** out,
                           size_t* count) {
    static const IhsMcpResourceDesc kResources[] = {
        {"fz://state", "state", "Fuzzing fixture state.", "application/json"},
    };
    *out = kResources;
    *count = sizeof(kResources) / sizeof(kResources[0]);
    return IHS_MCP_OK;
  }

  // The arguments are echoed back as an opaque string rather than re-parsed.
  // Re-parsing here would fuzz rapidjson, which is not what this target is
  // for; passing them through means whatever the transport extracted has to
  // survive being embedded in a reply.
  static int CallTool(void*,
                      const char* name,
                      const char* arguments_json,
                      const size_t arguments_length,
                      IhsMcpPayload* out) {
    static std::string reply;
    reply.assign(R"({"tool":")");
    reply.append(name != nullptr ? name : "");
    reply.append(R"(","argument_bytes":)");
    reply.append(std::to_string(arguments_length));
    reply.append(R"(,"arguments_were_null":)");
    reply.append(arguments_json == nullptr ? "true" : "false");
    reply.append("}");
    out->struct_size = sizeof(IhsMcpPayload);
    out->data = reply.c_str();
    out->length = reply.size();
    out->release = nullptr;
    out->release_ctx = nullptr;
    return IHS_MCP_OK;
  }

  static int ReadResource(void*, const char*, IhsMcpPayload* out) {
    static const char kBody[] = R"({"ok":true})";
    out->struct_size = sizeof(IhsMcpPayload);
    out->data = kBody;
    out->length = sizeof(kBody) - 1;
    out->release = nullptr;
    out->release_ctx = nullptr;
    return IHS_MCP_OK;
  }

  static int Subscribe(void*, const char*, bool) { return IHS_MCP_OK; }
};

bool RegisterProvider() {
  IhsMcpProviderDesc desc{};
  desc.struct_size = sizeof(desc);
  desc.name = "fuzz";
  desc.tool_prefix = "fz_";
  desc.resource_scheme = "fz://";
  desc.capability_mask = IHS_MCP_CAP_ALL;
  desc.notify_fd = -1;
  desc.user_data = nullptr;
  desc.callbacks.list_tools = &EchoProvider::ListTools;
  desc.callbacks.list_resources = &EchoProvider::ListResources;
  desc.callbacks.call_tool = &EchoProvider::CallTool;
  desc.callbacks.read_resource = &EchoProvider::ReadResource;
  desc.callbacks.subscribe = &EchoProvider::Subscribe;

  IhsMcpProvider* provider = nullptr;
  return ihs_mcp_provider_register(&desc, &provider) == IHS_MCP_OK;
}

bool StartTransport() {
  IhsMcpTransportConfig config{};
  config.struct_size = sizeof(config);
  config.socket_path = g_socket_path->c_str();
  config.max_request_bytes = 64 * 1024;
  return ihs_mcp_transport_start(&config) == IHS_MCP_OK;
}

int Connect() {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::snprintf(address.sun_path, sizeof(address.sun_path), "%s",
                g_socket_path->c_str());
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
      0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
  const char* tmp = ::getenv("TMPDIR");
  g_socket_path =
      new std::string(std::string(tmp != nullptr ? tmp : "/tmp") +
                      "/ihs-fuzz-mcp-" + std::to_string(::getpid()) + ".sock");
  if (!RegisterProvider() || !StartTransport()) {
    std::fprintf(stderr, "fuzz_mcp_transport: could not serve on %s\n",
                 g_socket_path->c_str());
    ::abort();  // running with no server under test would prove nothing
  }
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, const size_t size) {
  if (size == 0) {
    return 0;
  }
  const int fd = Connect();
  if (fd < 0) {
    return 0;
  }

  // A short write is not worth retrying: the point is what the server does
  // with what arrived, and a truncated request is itself a case under test.
  // MSG_NOSIGNAL rather than a SIGPIPE handler, so a peer the server hung up
  // on returns EPIPE here instead of killing the fuzzer mid-run.
  const ssize_t written = ::send(fd, data, size, MSG_NOSIGNAL);
  static_cast<void>(written);
  ::shutdown(fd, SHUT_WR);  // the EOF a bodyless read is waiting for

  // Drain the reply. Anything the server sends has to be read, or a large
  // response would leave bytes in the socket buffer and the close would reset
  // the connection before the server finished writing -- turning a real reply
  // path into an untested one.
  std::string reply;
  char buffer[4096];
  while (true) {
    pollfd poll_fd = {fd, POLLIN, 0};
    if (::poll(&poll_fd, 1, kReplyTimeoutMs) <= 0) {
      break;
    }
    const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
    if (got <= 0) {
      break;
    }
    reply.append(buffer, static_cast<size_t>(got));
    if (reply.size() > 1u << 20) {
      break;
    }
  }
  // Closing is all this does with a stream the input opened, deliberately.
  //
  // The first version of this driver restarted the transport whenever a reply
  // announced an event stream, because an abandoned stream's descriptor was
  // then held until a write to it failed -- which needs a notification that
  // may never arrive -- and millions of executions turned that into descriptor
  // exhaustion. The transport now reaps a hung-up stream on the accept
  // thread's next wake, so the workaround is gone. Leaving it in would have
  // been worse than useless: it papered over exactly the defect this target
  // exists to find, and would have gone on hiding a regression in the reaping.
  static_cast<void>(reply);
  return 0;
}
