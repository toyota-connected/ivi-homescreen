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

// MCP transport: JSON-RPC 2.0 over HTTP on a Unix domain socket. Framing and
// dispatch only -- every method here is a thin translation onto
// ihs_mcp_registry.h, which is where tools and resources actually live.

#include "ihs/ihs_mcp_transport.h"

#include "ihs/ihs_mcp_provider.h"
#include "ihs/ihs_mcp_registry.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include "ihs/logging.h"

namespace {

// Logging goes through the C API for the same reason the rest of ihs_shared
// does: the library sits below the shell's C++ logging wrapper.
int32_t TraceContext() {
  static const int32_t ctx = ihs_log_context_open("MCPT", nullptr);
  return ctx;
}

void Log(const int32_t level, const std::string& text) {
  const int32_t ctx = TraceContext();
  if (ctx < 0 || ihs_log_enabled(ctx, level) == 0) {
    return;
  }
  ihs_log(ctx, level, text.c_str(), text.size());
}

constexpr size_t kDefaultMaxRequestBytes = 1024 * 1024;

// Bounds the header section independently of the body: headers are read
// before any Content-Length is known, so without this a peer could stream
// header bytes forever and never be refused.
constexpr size_t kMaxHeaderBytes = 16 * 1024;

// The MCP revision this speaks. Sent back from initialize; a client asking for
// something else still gets this, which is what the specification calls for --
// the server states what it supports and the client decides whether to go on.
constexpr const char* kProtocolVersion = "2025-06-18";

// JSON-RPC 2.0 error codes.
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;

struct Transport {
  std::mutex mutex;
  bool running = false;
  int listen_fd = -1;
  int wake_fd = -1;  // eventfd-free: a self-pipe, so stop() interrupts accept
  int wake_write_fd = -1;
  std::string socket_path{};
  size_t max_request_bytes = kDefaultMaxRequestBytes;
  std::thread thread;

  // Open event streams, one per client that asked for one. Raw descriptors
  // because the only operations are write and close, and both happen under
  // this mutex: a notification arrives on the registry's watcher thread while
  // the accept loop may be adding a stream or dropping a dead one.
  //
  // Separate from `mutex` so delivering an event never contends with an
  // accept or a stop -- and, more importantly, so a notification cannot
  // deadlock against a stop that is waiting to join the accept thread.
  std::mutex streams_mutex;
  std::vector<int> streams;
};

Transport& TheTransport() {
  static auto* t = new Transport();  // leaked: outlives static teardown
  return *t;
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

std::string Escape(const std::string& in) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.String(in.c_str(), static_cast<rapidjson::SizeType>(in.size()));
  return buffer.GetString();
}

// The id is echoed verbatim so a client's own numbering survives, including
// the null the specification requires when a request could not be parsed well
// enough to have one.
std::string ResultEnvelope(const std::string& id, const std::string& result) {
  return R"({"jsonrpc":"2.0","id":)" + id + R"(,"result":)" + result + "}";
}

std::string ErrorEnvelope(const std::string& id,
                          const int code,
                          const std::string& message) {
  return R"({"jsonrpc":"2.0","id":)" + id + R"(,"error":{"code":)" +
         std::to_string(code) + R"(,"message":)" + Escape(message) + "}}";
}

// Maps a host status onto the JSON-RPC code that describes it. A denied
// capability is deliberately not folded into "not found": the host keeps them
// distinct so a masked tool asked for by name is visible as a refusal rather
// than a typo, and flattening that here would throw the distinction away at
// the last step.
int JsonRpcCodeFor(const int status) {
  if (status == IHS_MCP_ERR_NOT_FOUND) {
    return kMethodNotFound;
  }
  if (status == IHS_MCP_ERR_INVALID) {
    return kInvalidParams;
  }
  // A denied capability, a refusal and an unavailable provider are all the
  // server declining rather than the request being malformed, which is what
  // JSON-RPC's internal error covers. The distinction between them survives
  // in the message, which StatusText keeps specific.
  return kInternalError;
}

const char* StatusText(const int status) {
  switch (status) {
    case IHS_MCP_ERR_NOT_FOUND:
      return "no such tool or resource";
    case IHS_MCP_ERR_INVALID:
      return "invalid arguments";
    case IHS_MCP_ERR_CAPABILITY_DENIED:
      return "capability denied";
    case IHS_MCP_ERR_REFUSED:
      return "refused";
    case IHS_MCP_ERR_UNAVAILABLE:
      return "unavailable";
    default:
      return "internal error";
  }
}

// ---------------------------------------------------------------------------
// Method handlers
// ---------------------------------------------------------------------------

struct ListAccumulator {
  std::string out{};
  bool first = true;
};

void AppendTool(const IhsMcpRegistryTool* tool, void* user_data) {
  auto* acc = static_cast<ListAccumulator*>(user_data);
  if (!acc->first) {
    acc->out += ",";
  }
  acc->first = false;
  acc->out += R"({"name":)" + Escape(tool->name != nullptr ? tool->name : "");
  acc->out += R"(,"description":)" +
              Escape(tool->description != nullptr ? tool->description : "");
  // The schema is already JSON, so it is spliced rather than escaped. A
  // provider that registered something malformed would corrupt the envelope,
  // which is why the registry validates it at registration.
  acc->out += R"(,"inputSchema":)";
  acc->out +=
      (tool->input_schema_json != nullptr && tool->input_schema_json[0] != '\0')
          ? tool->input_schema_json
          : R"({"type":"object"})";
  acc->out += "}";
}

void AppendResource(const IhsMcpRegistryResource* resource, void* user_data) {
  auto* acc = static_cast<ListAccumulator*>(user_data);
  if (!acc->first) {
    acc->out += ",";
  }
  acc->first = false;
  acc->out +=
      R"({"uri":)" + Escape(resource->uri != nullptr ? resource->uri : "");
  acc->out +=
      R"(,"name":)" + Escape(resource->name != nullptr ? resource->name : "");
  acc->out +=
      R"(,"description":)" +
      Escape(resource->description != nullptr ? resource->description : "");
  acc->out += R"(,"mimeType":)" + Escape(resource->mime_type != nullptr
                                             ? resource->mime_type
                                             : "application/json");
  acc->out += "}";
}

std::string HandleInitialize() {
  // listChanged and subscribe are advertised now that an event stream can
  // carry them. They were deliberately absent while it could not: a client
  // told to expect notifications that never arrive waits instead of polling,
  // which is worse than being told nothing.
  return std::string(R"({"protocolVersion":")") + kProtocolVersion +
         R"(","capabilities":{"tools":{"listChanged":true},)" +
         R"("resources":{"listChanged":true,"subscribe":true}})" +
         R"(,"serverInfo":{"name":"ivi-homescreen","version":"1"}})";
}

std::string HandleToolsList() {
  ListAccumulator acc;
  acc.out = R"({"tools":[)";
  ihs_mcp_registry_for_each_tool(AppendTool, &acc);
  acc.out += "]}";
  return acc.out;
}

std::string HandleResourcesList() {
  ListAccumulator acc;
  acc.out = R"({"resources":[)";
  ihs_mcp_registry_for_each_resource(AppendResource, &acc);
  acc.out += "]}";
  return acc.out;
}

// Serializes the params sub-object back to text. The host takes tool arguments
// as JSON, so re-emitting is cheaper and safer than teaching it a value type.
std::string ReEmit(const rapidjson::Value& value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return buffer.GetString();
}

std::string HandleToolsCall(const rapidjson::Value& params,
                            const std::string& id) {
  if (!params.IsObject() || !params.HasMember("name") ||
      !params["name"].IsString()) {
    return ErrorEnvelope(id, kInvalidParams, "tools/call requires a name");
  }
  const std::string name = params["name"].GetString();

  std::string arguments = "{}";
  if (params.HasMember("arguments") && params["arguments"].IsObject()) {
    arguments = ReEmit(params["arguments"]);
  }

  IhsMcpPayload payload{};
  const int status = ihs_mcp_registry_call_tool(name.c_str(), arguments.c_str(),
                                                arguments.size(), &payload);
  if (status != IHS_MCP_OK) {
    ihs_mcp_registry_release_payload(&payload);
    return ErrorEnvelope(id, JsonRpcCodeFor(status), StatusText(status));
  }

  const std::string content(payload.data != nullptr ? payload.data : "",
                            payload.length);
  ihs_mcp_registry_release_payload(&payload);

  // The tool's own JSON is carried as MCP text content: providers return a
  // result document, and the specification's content array is the envelope a
  // client expects to unwrap.
  return ResultEnvelope(id, R"({"content":[{"type":"text","text":)" +
                                Escape(content) + R"(}],"isError":false})");
}

std::string HandleResourcesRead(const rapidjson::Value& params,
                                const std::string& id) {
  if (!params.IsObject() || !params.HasMember("uri") ||
      !params["uri"].IsString()) {
    return ErrorEnvelope(id, kInvalidParams, "resources/read requires a uri");
  }
  const std::string uri = params["uri"].GetString();

  IhsMcpPayload payload{};
  const int status = ihs_mcp_registry_read_resource(uri.c_str(), &payload);
  if (status != IHS_MCP_OK) {
    ihs_mcp_registry_release_payload(&payload);
    return ErrorEnvelope(id, JsonRpcCodeFor(status), StatusText(status));
  }

  const std::string content(payload.data != nullptr ? payload.data : "",
                            payload.length);
  ihs_mcp_registry_release_payload(&payload);

  return ResultEnvelope(id, R"({"contents":[{"uri":)" + Escape(uri) +
                                R"(,"mimeType":"application/json","text":)" +
                                Escape(content) + "}]}");
}

// Routes one JSON-RPC request. Returns the response text, or empty for a
// notification -- a request with no id expects no reply, and answering one
// would be a protocol error rather than merely noise.
std::string Dispatch(const std::string& body) {
  rapidjson::Document doc;
  if (doc.Parse(body.c_str(), body.size()).HasParseError() || !doc.IsObject()) {
    return ErrorEnvelope("null", kParseError, "invalid JSON");
  }

  std::string id = "null";
  bool is_notification = true;
  if (doc.HasMember("id") && !doc["id"].IsNull()) {
    id = ReEmit(doc["id"]);
    is_notification = false;
  }

  if (!doc.HasMember("method") || !doc["method"].IsString()) {
    return is_notification
               ? std::string()
               : ErrorEnvelope(id, kInvalidRequest, "missing method");
  }
  const std::string method = doc["method"].GetString();

  static const rapidjson::Value kEmpty(rapidjson::kObjectType);
  const rapidjson::Value& params =
      doc.HasMember("params") ? doc["params"] : kEmpty;

  std::string response;
  if (method == "initialize") {
    response = ResultEnvelope(id, HandleInitialize());
  } else if (method == "tools/list") {
    response = ResultEnvelope(id, HandleToolsList());
  } else if (method == "resources/list") {
    response = ResultEnvelope(id, HandleResourcesList());
  } else if (method == "tools/call") {
    response = HandleToolsCall(params, id);
  } else if (method == "resources/read") {
    response = HandleResourcesRead(params, id);
  } else if (method == "ping") {
    response = ResultEnvelope(id, "{}");
  } else if (method == "notifications/initialized") {
    return {};  // a notification by definition; nothing to answer
  } else {
    response = ErrorEnvelope(id, kMethodNotFound, "unknown method: " + method);
  }

  return is_notification ? std::string() : response;
}

// ---------------------------------------------------------------------------
// HTTP framing
// ---------------------------------------------------------------------------

std::string HttpResponse(const int code,
                         const char* reason,
                         const std::string& body) {
  std::string out = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
  out += "Content-Type: application/json\r\n";
  out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  // Nothing here is cacheable and every response is a live view of the UI.
  out += "Cache-Control: no-store\r\n";
  out += "Connection: close\r\n\r\n";
  out += body;
  return out;
}

bool WriteAll(const int fd, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n = ::write(fd, data.data() + sent, data.size() - sent);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

// Reads until the header terminator and no further, for a request with no
// body. Bounded like ReadRequest, so a peer cannot stream headers forever.
bool ReadHeaderBlock(const int fd, std::string* out_headers) {
  std::string buffer;
  while (buffer.find("\r\n\r\n") == std::string::npos) {
    char chunk[1024];
    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n <= 0) {
      if (n < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    buffer.append(chunk, static_cast<size_t>(n));
    if (buffer.size() > kMaxHeaderBytes) {
      WriteAll(fd, HttpResponse(431, "Request Header Fields Too Large", "{}"));
      return false;
    }
  }
  *out_headers = std::move(buffer);
  return true;
}

// Reads until the header terminator, then exactly Content-Length bytes.
// Returns false when the peer is malformed or over budget, having already
// written the refusal.
bool ReadRequest(const int fd, const size_t max_body, std::string* out_body) {
  std::string buffer;
  size_t header_end = std::string::npos;

  while (true) {
    char chunk[4096];
    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;  // peer closed before completing the request
    }
    buffer.append(chunk, static_cast<size_t>(n));

    header_end = buffer.find("\r\n\r\n");
    if (header_end != std::string::npos) {
      break;
    }
    if (buffer.size() > kMaxHeaderBytes) {
      WriteAll(fd, HttpResponse(431, "Request Header Fields Too Large", "{}"));
      return false;
    }
  }

  // Only POST carries a body, and every JSON-RPC call is a POST. Anything else
  // is answered rather than ignored, so a browser or a probe gets a clear no.
  if (buffer.rfind("POST ", 0) != 0) {
    WriteAll(
        fd, HttpResponse(405, "Method Not Allowed", R"({"error":"use POST"})"));
    return false;
  }

  // Header names are case-insensitive, so match on a lowered copy while
  // keeping the original for the body split.
  std::string lowered = buffer.substr(0, header_end);
  for (char& c : lowered) {
    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  }

  size_t content_length = 0;
  const size_t cl = lowered.find("content-length:");
  if (cl == std::string::npos) {
    WriteAll(fd, HttpResponse(411, "Length Required",
                              R"({"error":"Content-Length required"})"));
    return false;
  }
  {
    const size_t value_start = cl + std::strlen("content-length:");
    const size_t line_end = lowered.find("\r\n", value_start);
    const std::string value =
        lowered.substr(value_start, line_end - value_start);
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str()) {
      WriteAll(fd, HttpResponse(400, "Bad Request",
                                R"({"error":"bad Content-Length"})"));
      return false;
    }
    content_length = static_cast<size_t>(parsed);
  }

  // Refused on the declared length, before reading it: an oversized body
  // should cost the shell nothing to turn away.
  if (content_length > max_body) {
    WriteAll(fd, HttpResponse(413, "Content Too Large",
                              R"({"error":"request too large"})"));
    return false;
  }

  std::string body = buffer.substr(header_end + 4);
  while (body.size() < content_length) {
    char chunk[4096];
    const size_t want = std::min(sizeof(chunk), content_length - body.size());
    const ssize_t n = ::read(fd, chunk, want);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;
    }
    body.append(chunk, static_cast<size_t>(n));
  }
  body.resize(content_length);

  *out_body = std::move(body);
  return true;
}

// The socket is 0600, so the filesystem already keeps other users out. This
// is the second half of that: it rejects a peer running as a different uid
// that reached the socket anyway -- through an inherited descriptor, or a
// directory whose permissions were loosened after the fact.
bool PeerIsOwner(const int fd) {
  ucred credentials{};
  socklen_t length = sizeof(credentials);
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
    return false;  // cannot establish who this is, so do not serve them
  }
  return credentials.uid == ::geteuid();
}

// One server-sent event. The blank line terminates it; without it a client
// buffers the payload and delivers nothing.
std::string EventFrame(const std::string& json) {
  return "data: " + json + "\n\n";
}

// Pushes to every open stream, dropping any that will not take it.
//
// A stream is non-blocking, so a client that has stopped reading shows up as
// EAGAIN rather than stalling this thread -- which matters because this runs
// on the registry's watcher thread, and blocking here would stop every other
// provider's notifications too. Such a client is dropped: a partial frame
// would desynchronise the stream, and there is no way to resend.
void Broadcast(const std::string& json) {
  Transport& t = TheTransport();
  const std::string frame = EventFrame(json);

  const std::lock_guard<std::mutex> lock(t.streams_mutex);
  auto it = t.streams.begin();
  while (it != t.streams.end()) {
    size_t sent = 0;
    bool alive = true;
    while (sent < frame.size()) {
      const ssize_t n = ::write(*it, frame.data() + sent, frame.size() - sent);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        alive = false;  // EAGAIN included: a client not reading is gone to us
        break;
      }
      sent += static_cast<size_t>(n);
    }
    if (alive) {
      ++it;
      continue;
    }
    ::close(*it);
    it = t.streams.erase(it);
  }
}

// The registry calls this when a provider changes. Translated into the
// JSON-RPC notifications the specification names, and pushed to every stream.
void OnRegistryNotification(const IhsMcpNotification kind,
                            const char* uri,
                            void* /* user_data */) {
  if (kind == IHS_MCP_NOTIFY_TOOLS_CHANGED) {
    Broadcast(
        R"({"jsonrpc":"2.0","method":"notifications/tools/list_changed"})");
    return;
  }
  const std::string resource = uri != nullptr ? uri : "";
  Broadcast(R"({"jsonrpc":"2.0","method":"notifications/resources/updated")"
            R"(,"params":{"uri":)" +
            Escape(resource) + "}}");
}

// Turns an accepted connection into an event stream. Returns false when the
// client did not ask for one, leaving it to be served as an ordinary request.
bool TryOpenEventStream(const int fd, const std::string& headers) {
  // A GET asking for the event-stream media type is how MCP's Streamable HTTP
  // transport opens the server-to-client direction.
  if (headers.rfind("GET ", 0) != 0) {
    return false;
  }
  std::string lowered = headers;
  for (char& c : lowered) {
    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  }
  if (lowered.find("text/event-stream") == std::string::npos) {
    WriteAll(fd, HttpResponse(
                     406, "Not Acceptable",
                     R"({"error":"GET requires Accept: text/event-stream"})"));
    return true;  // answered; caller should close
  }

  std::string response = "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: text/event-stream\r\n";
  response += "Cache-Control: no-store\r\n";
  // No Content-Length and no close: the body is the stream, and it ends when
  // one side hangs up.
  response += "Connection: keep-alive\r\n\r\n";
  if (!WriteAll(fd, response)) {
    return true;
  }

  // Non-blocking from here on, so a client that stops reading cannot stall
  // the thread that delivers notifications.
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  Transport& t = TheTransport();
  const std::lock_guard<std::mutex> lock(t.streams_mutex);
  t.streams.push_back(fd);
  return true;
}

// Serves one connection. Returns true when the descriptor has become an event
// stream and must stay open; the caller closes it otherwise.
bool ServeConnection(const int fd, const size_t max_body) {
  if (!PeerIsOwner(fd)) {
    Log(IHS_LEVEL_WARN, "mcp: refused a connection from another user");
    WriteAll(fd, HttpResponse(403, "Forbidden", R"({"error":"forbidden"})"));
    return false;
  }

  // Peeked rather than read: a GET opens an event stream and a POST carries a
  // request, and only the latter has a body to consume. Peeking keeps the
  // request-reading path below exactly as it was.
  char probe[4] = {0, 0, 0, 0};
  const ssize_t peeked = ::recv(fd, probe, sizeof(probe), MSG_PEEK);
  if (peeked <= 0) {
    return false;
  }
  if (std::strncmp(probe, "GET ", 4) == 0) {
    std::string headers;
    if (!ReadHeaderBlock(fd, &headers)) {
      return false;
    }
    if (TryOpenEventStream(fd, headers)) {
      Transport& t = TheTransport();
      const std::lock_guard<std::mutex> lock(t.streams_mutex);
      // Kept open only if it actually joined the registry; a refused GET was
      // answered and is finished with.
      return std::find(t.streams.begin(), t.streams.end(), fd) !=
             t.streams.end();
    }
    return false;
  }

  std::string body;
  if (!ReadRequest(fd, max_body, &body)) {
    return false;  // ReadRequest already answered, or the peer went away
  }

  const std::string response = Dispatch(body);
  if (response.empty()) {
    // A notification. HTTP still needs a reply, and 202 says "taken, nothing
    // to return" without inventing a JSON-RPC response the client must ignore.
    WriteAll(fd, HttpResponse(202, "Accepted", ""));
    return false;
  }
  WriteAll(fd, HttpResponse(200, "OK", response));
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

namespace {

std::string DefaultSocketPath() {
  const char* runtime_dir = ::getenv("XDG_RUNTIME_DIR");
  std::string base;
  if (runtime_dir != nullptr && runtime_dir[0] != '\0') {
    base = runtime_dir;
  } else {
    base = "/run/user/" + std::to_string(::geteuid());
  }
  return base + "/ivi-homescreen/mcp.sock";
}

// Creates the socket's parent 0700. The directory is as much of the boundary
// as the socket mode is: a world-writable parent would let another user
// replace the socket with their own.
bool EnsureParentDirectory(const std::string& path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0) {
    return true;
  }
  const std::string parent = path.substr(0, slash);
  if (::mkdir(parent.c_str(), 0700) == 0) {
    return true;
  }
  return errno == EEXIST;
}

// Tells a stale socket from a live one by trying to connect. Only a socket
// nobody answers is removed, so two shells racing for the same path produce a
// refusal rather than one silently taking the other's clients.
bool StaleSocketRemoved(const std::string& path) {
  const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (probe < 0) {
    return false;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);

  const bool live = ::connect(probe, reinterpret_cast<sockaddr*>(&address),
                              sizeof(address)) == 0;
  ::close(probe);
  if (live) {
    return false;  // someone is serving here; leave them alone
  }
  ::unlink(path.c_str());
  return true;
}

void AcceptLoop() {
  Transport& t = TheTransport();
  const int listen_fd = t.listen_fd;
  const int wake_fd = t.wake_fd;
  const size_t max_body = t.max_request_bytes;

  while (true) {
    pollfd fds[2];
    fds[0] = {listen_fd, POLLIN, 0};
    fds[1] = {wake_fd, POLLIN, 0};
    const int ready = ::poll(fds, 2, -1);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if ((fds[1].revents & POLLIN) != 0) {
      break;  // stop() asked
    }
    if ((fds[0].revents & POLLIN) == 0) {
      continue;
    }

    const int client = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      break;
    }
    // Served inline. Requests are short and the host serializes anyway, so a
    // thread per connection would buy concurrency the registry cannot use.
    // A connection that became an event stream stays open and is owned by
    // the stream registry from here.
    if (!ServeConnection(client, max_body)) {
      ::close(client);
    }
  }
}

}  // namespace

int ihs_mcp_transport_start(const IhsMcpTransportConfig* config) {
  if (config == nullptr || config->struct_size == 0) {
    return IHS_MCP_ERR_INVALID;
  }

  Transport& t = TheTransport();
  const std::lock_guard<std::mutex> lock(t.mutex);
  if (t.running) {
    return IHS_MCP_OK;  // idempotent
  }

  const std::string path =
      (config->socket_path != nullptr && config->socket_path[0] != '\0')
          ? config->socket_path
          : DefaultSocketPath();

  if (path.size() >= sizeof(sockaddr_un::sun_path)) {
    Log(IHS_LEVEL_ERROR, "mcp: socket path is too long: " + path);
    return IHS_MCP_ERR_INVALID;
  }
  if (!EnsureParentDirectory(path)) {
    Log(IHS_LEVEL_ERROR, "mcp: cannot create the socket directory for " + path +
                             ": " + std::strerror(errno));
    return IHS_MCP_ERR_UNAVAILABLE;
  }
  if (::access(path.c_str(), F_OK) == 0 && !StaleSocketRemoved(path)) {
    Log(IHS_LEVEL_ERROR, "mcp: " + path + " is already being served");
    return IHS_MCP_ERR_REFUSED;
  }

  const int listen_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd < 0) {
    Log(IHS_LEVEL_ERROR, std::string("mcp: socket(): ") + std::strerror(errno));
    return IHS_MCP_ERR_UNAVAILABLE;
  }

  // Narrow the mode across bind() rather than chmod'ing afterwards: between a
  // permissive bind and a later chmod the socket is briefly connectable by
  // anyone, and that window is the whole control.
  const mode_t previous_umask = ::umask(0177);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  const bool bound = ::bind(listen_fd, reinterpret_cast<sockaddr*>(&address),
                            sizeof(address)) == 0;
  ::umask(previous_umask);

  if (!bound) {
    Log(IHS_LEVEL_ERROR, "mcp: bind(" + path + "): " + std::strerror(errno));
    ::close(listen_fd);
    return IHS_MCP_ERR_UNAVAILABLE;
  }
  if (::listen(listen_fd, 4) != 0) {
    Log(IHS_LEVEL_ERROR, std::string("mcp: listen(): ") + std::strerror(errno));
    ::close(listen_fd);
    ::unlink(path.c_str());
    return IHS_MCP_ERR_UNAVAILABLE;
  }

  int wake[2] = {-1, -1};
  if (::pipe(wake) != 0) {
    Log(IHS_LEVEL_ERROR, std::string("mcp: pipe(): ") + std::strerror(errno));
    ::close(listen_fd);
    ::unlink(path.c_str());
    return IHS_MCP_ERR_UNAVAILABLE;
  }

  t.listen_fd = listen_fd;
  t.wake_fd = wake[0];
  t.wake_write_fd = wake[1];
  t.socket_path = path;
  t.max_request_bytes = config->max_request_bytes != 0
                            ? config->max_request_bytes
                            : kDefaultMaxRequestBytes;
  t.running = true;
  t.thread = std::thread(AcceptLoop);

  // Installed last: the registry starts watching provider descriptors the
  // moment this returns, and a notification arriving before there is anywhere
  // to put it would be dropped rather than queued.
  //
  // Not fatal if refused -- something else already owns the sink, which means
  // requests still work and only pushed notifications are missing. Said out
  // loud, because a client that saw listChanged advertised will wait for
  // messages that then never come.
  if (ihs_mcp_registry_set_notification_sink(OnRegistryNotification, nullptr) !=
      IHS_MCP_OK) {
    Log(IHS_LEVEL_ERROR,
        "mcp: another notification sink is installed; serving requests "
        "without pushed notifications");
  }

  Log(IHS_LEVEL_INFO, "mcp: serving on " + path);
  return IHS_MCP_OK;
}

void ihs_mcp_transport_stop() {
  Transport& t = TheTransport();

  // Uninstalled first, and this does not return until the registry's watcher
  // is joined -- so no notification can be in flight into Broadcast while the
  // streams below are being closed.
  ihs_mcp_registry_set_notification_sink(nullptr, nullptr);

  std::thread thread;
  {
    const std::lock_guard<std::mutex> lock(t.mutex);
    if (!t.running) {
      return;
    }
    t.running = false;
    const char byte = 1;
    ssize_t written;
    do {
      written = ::write(t.wake_write_fd, &byte, 1);
    } while (written < 0 && errno == EINTR);
    thread = std::move(t.thread);
  }

  if (thread.joinable()) {
    thread.join();
  }

  const std::lock_guard<std::mutex> lock(t.mutex);
  ::close(t.listen_fd);
  ::close(t.wake_fd);
  ::close(t.wake_write_fd);
  t.listen_fd = -1;
  t.wake_fd = -1;
  t.wake_write_fd = -1;
  ::unlink(t.socket_path.c_str());
  t.socket_path.clear();

  // Closing a stream is how a client learns the server is gone: there is no
  // "goodbye" event, and the specification expects the transport to end.
  const std::lock_guard<std::mutex> streams_lock(t.streams_mutex);
  for (const int stream : t.streams) {
    ::close(stream);
  }
  t.streams.clear();
}

bool ihs_mcp_transport_running() {
  Transport& t = TheTransport();
  const std::lock_guard<std::mutex> lock(t.mutex);
  return t.running;
}

const char* ihs_mcp_transport_socket_path() {
  Transport& t = TheTransport();
  const std::lock_guard<std::mutex> lock(t.mutex);
  return t.socket_path.c_str();
}
