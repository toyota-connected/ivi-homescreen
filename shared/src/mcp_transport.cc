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
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <set>
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

// Bounds on what one client can make this transport hold on to.
//
// Every request is Connection: close, so a connection is not a lifetime and
// there is nothing for the state below to be scoped to. Left unbounded, a
// client that keeps asking makes the shell keep holding: open event streams
// cost a descriptor each, and subscriptions cost memory keyed by strings the
// client chose. Neither is reclaimed by anything the client does on its way
// out, because a client that goes away does not say so.
//
// The numbers are generous against the real shape of this: an instrument
// cluster is driven by one agent, occasionally a handful. Anything that
// exceeds these is not a client that has been running a long time, it is a
// client in a loop.
constexpr size_t kMaxStreams = 32;
constexpr size_t kMaxSessions = 64;
constexpr size_t kMaxSubscriptionsPerSession = 64;

// A resource URI is a name. The body limit alone would let one be a megabyte,
// and it would then be held for as long as the session is.
constexpr size_t kMaxUriBytes = 512;

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

// One open event stream. The session is empty when the client opened the
// stream without identifying one, which is legal and means "send me
// everything" -- see Transport::subscriptions.
struct Stream {
  int fd = -1;
  std::string session{};
};

struct Transport {
  std::mutex mutex;
  bool running = false;
  int listen_fd = -1;
  int wake_fd = -1;  // eventfd-free: a self-pipe, so stop() interrupts accept
  int wake_write_fd = -1;
  std::string socket_path{};
  size_t max_request_bytes = kDefaultMaxRequestBytes;
  std::thread thread;

  // Open event streams, one per client that asked for one.
  //
  // Separate from `mutex` so delivering an event never contends with an
  // accept or a stop -- and, more importantly, so a notification cannot
  // deadlock against a stop that is waiting to join the accept thread.
  //
  // This mutex also covers `subscriptions`: a subscribe arrives on the accept
  // thread while a notification is being filtered on the registry's watcher
  // thread, so the set a stream is matched against must not change underneath
  // that match.
  std::mutex streams_mutex;
  std::vector<Stream> streams;

  // Session id -> the resource URIs that session asked for.
  //
  // Keyed by session rather than by stream because a subscribe arrives on its
  // own connection: every request here is Connection: close, so the POST that
  // subscribes is never the connection the stream is on, and the session id is
  // the only thing tying them together.
  //
  // A session absent from this map, or present with an empty set, receives
  // every resource update. That is what a client which never subscribed got
  // before subscriptions existed, and keeping it means adding this could not
  // silently stop an existing client's notifications -- "subscribed to
  // nothing" and "not interested in filtering" are not distinguishable, and
  // the safe reading is the one that keeps delivering.
  std::map<std::string, std::set<std::string>> subscriptions;

  // Session ids this transport minted, least recently used first.
  //
  // A subscription may only name one of these. Without the check the id is
  // whatever the client put in the header, so "session" would be a namespace
  // the client writes into at will -- unbounded, and shared, which also lets
  // one client unsubscribe another's URIs by naming its id.
  //
  // Bounded, and evicted least-recently-used rather than oldest-first: a
  // long-lived agent stays at the back because every request it makes touches
  // its id, so what falls off the front is a session nothing has spoken for.
  // Eviction drops that session's filter, which means it receives everything
  // rather than nothing -- the same reading as a session that never
  // subscribed, and the safe direction to be wrong in.
  std::deque<std::string> sessions;

  // Notification debounce.
  //
  // A provider signals per republished tree, so an animating UI produces one
  // notification per frame. These carry no state beyond "something changed" --
  // a client re-reads to find out what -- so collapsing a burst into one loses
  // nothing, provided the last one still arrives. A client that misses the
  // trailing edge is left believing a stale tree is current, with nothing to
  // tell it otherwise, which is the failure this has to avoid rather than the
  // volume.
  std::mutex notify_mutex;
  std::condition_variable notify_cv;
  bool notify_running = false;
  std::thread notify_thread;
  bool tools_pending = false;
  std::set<std::string> resources_pending;
  std::chrono::steady_clock::time_point tools_last{};
  std::map<std::string, std::chrono::steady_clock::time_point> resource_last;
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

// Whether the tool ran and reported a failure, as opposed to the call never
// reaching one.
//
// The dividing line is whether the tool's own code had a say. An unknown tool,
// a malformed call and a capability the consumer does not hold are all decided
// before anything executes, so they are protocol errors. A refusal or an
// unavailable provider is the tool answering -- including an application that
// did not answer in time, which is a fact about running it rather than about
// the request.
bool ToolExecuted(const int status) {
  return status == IHS_MCP_ERR_REFUSED || status == IHS_MCP_ERR_UNAVAILABLE;
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
  std::string content(payload.data != nullptr ? payload.data : "",
                      payload.length);
  ihs_mcp_registry_release_payload(&payload);

  // A tool that ran and failed is not a protocol error, and the specification
  // separates the two deliberately: an unknown tool or a malformed call is a
  // JSON-RPC error, while a tool that executed and could not do the thing is a
  // result with isError set. Collapsing them loses the failure's own
  // explanation -- which for a tool the application declared is the only thing
  // saying what went wrong, since the host knows nothing about what it does.
  if (status != IHS_MCP_OK && !ToolExecuted(status)) {
    return ErrorEnvelope(id, JsonRpcCodeFor(status), StatusText(status));
  }

  const bool failed = status != IHS_MCP_OK;
  if (failed && content.empty()) {
    // Nothing said why, so say what is known rather than returning an empty
    // failure a client cannot act on.
    content = std::string(R"({"error":)") + Escape(StatusText(status)) + "}";
  }

  // The tool's own JSON is carried as MCP text content: providers return a
  // result document, and the specification's content array is the envelope a
  // client expects to unwrap.
  return ResultEnvelope(id, R"({"content":[{"type":"text","text":)" +
                                Escape(content) + R"(}],"isError":)" +
                                (failed ? "true" : "false") + "}");
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
// A session identifier: 128 bits of randomness, hex.
//
// Random rather than a counter because the id is what authorizes a subscribe
// to alter a session's filter. The socket's uid check is the real boundary and
// this sits behind it, but a guessable id would let anything that got through
// that boundary reshape another client's stream, and the cost of not being
// guessable is one read.
std::string NewSessionId() {
  std::random_device source;
  std::string out;
  out.reserve(32);
  for (int i = 0; i < 4; ++i) {
    const uint32_t word = source();
    for (int shift = 28; shift >= 0; shift -= 4) {
      out.push_back("0123456789abcdef"[(word >> shift) & 0xfu]);
    }
  }
  return out;
}

// Records a freshly minted session, evicting the least recently used one if
// that puts the count over the cap.
//
// Called with streams_mutex held, which is the lock covering both the session
// list and the subscriptions keyed by it -- the two have to move together or
// an evicted session leaves its filter behind, which is the leak this exists
// to close.
void RememberSessionLocked(Transport& t, const std::string& session) {
  t.sessions.push_back(session);
  while (t.sessions.size() > kMaxSessions) {
    t.subscriptions.erase(t.sessions.front());
    t.sessions.pop_front();
  }
}

// Whether this transport minted `session`, moving it to the most-recently-used
// end when it did. Called with streams_mutex held.
//
// The touch is what makes the cap survivable: without it the list is
// oldest-first, and an agent that has been connected all day is evicted by
// sixty-four initialize calls from anything else.
bool TouchSessionLocked(Transport& t, const std::string& session) {
  const auto it = std::find(t.sessions.begin(), t.sessions.end(), session);
  if (it == t.sessions.end()) {
    return false;
  }
  t.sessions.erase(it);
  t.sessions.push_back(session);
  return true;
}

// resources/subscribe and resources/unsubscribe.
//
// The session comes from the Mcp-Session-Id header rather than the params, and
// is checked against the ids this transport actually minted. The header is no
// less client-controlled than the body would be, so taking it from there is
// not by itself a protection -- the check is. Without it "session" is a
// namespace the client writes into: unbounded, and shared, so one client can
// name another's id and drop its subscriptions.
std::string HandleSubscribe(const rapidjson::Value& params,
                            const std::string& id,
                            const std::string& session,
                            const bool subscribing) {
  if (!params.IsObject() || !params.HasMember("uri") ||
      !params["uri"].IsString()) {
    return ErrorEnvelope(id, kInvalidParams, "uri is required");
  }
  if (session.empty()) {
    // Without a session there is nothing to attach the filter to: the stream
    // this would govern is a different connection, and only the session id
    // links them.
    return ErrorEnvelope(id, kInvalidParams,
                         "Mcp-Session-Id is required; call initialize first");
  }

  const std::string uri = params["uri"].GetString();
  if (uri.size() > kMaxUriBytes) {
    return ErrorEnvelope(
        id, kInvalidParams,
        "uri is longer than " + std::to_string(kMaxUriBytes) + " bytes");
  }

  // Pass it to the registry so the provider owning the scheme can start or
  // stop work on the strength of it.
  //
  // A scheme no provider claims is deliberately not an error. Providers
  // register at any point in a session -- that is an explicit property of the
  // registry, not an accident -- so refusing here would turn "subscribed
  // before the provider came up" into a failure, and make the result depend on
  // a race the client cannot see. The subscription is recorded either way and
  // starts matching when the provider appears. Anything other than a missing
  // provider is a real failure and is reported.
  const int status = ihs_mcp_registry_subscribe(uri.c_str(), subscribing);
  if (status != IHS_MCP_OK && status != IHS_MCP_ERR_NOT_FOUND) {
    return ErrorEnvelope(id, JsonRpcCodeFor(status), StatusText(status));
  }

  Transport& t = TheTransport();
  const std::lock_guard<std::mutex> lock(t.streams_mutex);
  if (!TouchSessionLocked(t, session)) {
    return ErrorEnvelope(id, kInvalidParams,
                         "unknown Mcp-Session-Id; call initialize first");
  }
  if (subscribing) {
    auto& uris = t.subscriptions[session];
    if (uris.size() >= kMaxSubscriptionsPerSession && uris.count(uri) == 0) {
      // Refused rather than evicting one of its own: which URI a client cares
      // about is its decision, and silently dropping one would leave it
      // believing it is still being told about a resource it is not.
      return ErrorEnvelope(id, kInvalidParams,
                           "session already holds " +
                               std::to_string(kMaxSubscriptionsPerSession) +
                               " subscriptions");
    }
    uris.insert(uri);
  } else {
    const auto it = t.subscriptions.find(session);
    if (it != t.subscriptions.end()) {
      it->second.erase(uri);
      // An empty set would mean "send everything" rather than "send nothing",
      // so unsubscribing from the last URI has to drop the session's filter
      // entirely rather than leave one that reads as the opposite.
      if (it->second.empty()) {
        t.subscriptions.erase(it);
      }
    }
  }
  return ResultEnvelope(id, "{}");
}

// Whether a notification for `notified` concerns a subscription to
// `subscribed`, which is true when either contains the other.
//
// Both directions are needed because the two names are not at the same
// granularity. A provider signals that something under its scheme changed and
// the registry reports the provider's prefix -- "ui://" -- while a client
// subscribes to what resources/list advertises, which is the concrete
// "ui://semantics/tree". Comparing those for equality would match nothing and
// filter out every real subscriber's notifications.
//
// So the coarser name governs: a notification for "ui://" reaches a
// subscription to "ui://semantics/tree", and a notification for a concrete URI
// reaches a subscription to the scheme. The practical resolution is therefore
// per-provider, not per-resource, and it sharpens on its own if providers
// start reporting the resource that actually changed.
bool UriConcerns(const std::string& notified, const std::string& subscribed) {
  const std::string& shorter =
      notified.size() <= subscribed.size() ? notified : subscribed;
  const std::string& longer =
      notified.size() <= subscribed.size() ? subscribed : notified;
  return longer.compare(0, shorter.size(), shorter) == 0;
}

// Whether a stream should receive an update for `uri`. Caller holds
// streams_mutex.
bool StreamWantsLocked(const Transport& t,
                       const Stream& stream,
                       const std::string& uri) {
  if (stream.session.empty()) {
    return true;  // never identified a session, so never asked to filter
  }
  const auto it = t.subscriptions.find(stream.session);
  if (it == t.subscriptions.end() || it->second.empty()) {
    return true;  // identified, but subscribed to nothing: unfiltered
  }
  return std::any_of(
      it->second.begin(), it->second.end(),
      [&uri](const std::string& want) { return UriConcerns(uri, want); });
}

std::string Dispatch(const std::string& body,
                     const std::string& session,
                     std::string* out_new_session) {
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
    // A session is minted here rather than on connect: every request is
    // Connection: close, so connections are not sessions, and initialize is
    // the one point the protocol defines for establishing one.
    *out_new_session = NewSessionId();
    {
      Transport& t = TheTransport();
      const std::lock_guard<std::mutex> lock(t.streams_mutex);
      RememberSessionLocked(t, *out_new_session);
    }
    response = ResultEnvelope(id, HandleInitialize());
  } else if (method == "tools/list") {
    response = ResultEnvelope(id, HandleToolsList());
  } else if (method == "resources/list") {
    response = ResultEnvelope(id, HandleResourcesList());
  } else if (method == "tools/call") {
    response = HandleToolsCall(params, id);
  } else if (method == "resources/subscribe") {
    response = HandleSubscribe(params, id, session, /*subscribing=*/true);
  } else if (method == "resources/unsubscribe") {
    response = HandleSubscribe(params, id, session, /*subscribing=*/false);
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
                         const std::string& body,
                         const std::string& extra_headers = {}) {
  std::string out = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
  out += "Content-Type: application/json\r\n";
  out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  // Nothing here is cacheable and every response is a live view of the UI.
  out += "Cache-Control: no-store\r\n";
  out += extra_headers;
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

// The value of a header, or "" when absent. `name` must be lowercase and end
// with ':'. Matches on a lowered copy because header names are
// case-insensitive, and trims the optional leading space and the trailing CR.
std::string HeaderValue(const std::string& raw_headers, const char* name) {
  std::string lowered = raw_headers;
  for (char& c : lowered) {
    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  }
  const std::string needle = std::string("\r\n") + name;
  const size_t at = lowered.find(needle);
  if (at == std::string::npos) {
    return {};
  }
  size_t start = at + needle.size();
  while (start < raw_headers.size() && raw_headers[start] == ' ') {
    ++start;
  }
  const size_t end = raw_headers.find("\r\n", start);
  return raw_headers.substr(
      start, end == std::string::npos ? std::string::npos : end - start);
}

// True when the request carries an Origin header.
//
// Only a browser sets Origin, and no browser is a legitimate client of this
// surface, so the header's presence is a reliable signal of DNS rebinding: a
// page the user happens to visit resolves a name it controls to a local
// address, posts to this endpoint, and the browser attaches the page's origin.
// The MCP specification requires HTTP transports to validate Origin for
// exactly this reason.
//
// A browser cannot open a Unix socket, so nothing can reach this today. It is
// here because the check belongs with the protocol rather than with the
// socket: anything that terminates a network transport in front of this one
// forwards plain HTTP into it, and this is what keeps a rebound request from
// arriving as an ordinary local one.
bool CarriesOrigin(const std::string& raw_headers) {
  std::string lowered = raw_headers;
  for (char& c : lowered) {
    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  }
  return lowered.find("\r\norigin:") != std::string::npos;
}

// Reads until the header terminator, then exactly Content-Length bytes.
// Returns false when the peer is malformed or over budget, having already
// written the refusal.
bool ReadRequest(const int fd,
                 const size_t max_body,
                 std::string* out_body,
                 std::string* out_headers) {
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

  if (CarriesOrigin(buffer.substr(0, header_end))) {
    Log(IHS_LEVEL_WARN, "mcp: refused a request carrying an Origin header");
    WriteAll(fd, HttpResponse(403, "Forbidden",
                              R"({"error":"origin not allowed"})"));
    return false;
  }

  *out_headers = buffer.substr(0, header_end);

  // Header names are case-insensitive, so match on a lowered copy while
  // keeping the original for the body split.
  std::string lowered = *out_headers;
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
// `uri` empty means the event is not resource-scoped (a list_changed), which
// every stream receives: there is nothing to filter it against.
void Broadcast(const std::string& json, const std::string& uri = {}) {
  Transport& t = TheTransport();
  const std::string frame = EventFrame(json);

  const std::lock_guard<std::mutex> lock(t.streams_mutex);
  auto it = t.streams.begin();
  while (it != t.streams.end()) {
    if (!uri.empty() && !StreamWantsLocked(t, *it, uri)) {
      ++it;
      continue;
    }
    size_t sent = 0;
    bool alive = true;
    while (sent < frame.size()) {
      const ssize_t n =
          ::write(it->fd, frame.data() + sent, frame.size() - sent);
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
    ::close(it->fd);
    it = t.streams.erase(it);
  }
}

// The registry calls this when a provider changes. Translated into the
// JSON-RPC notifications the specification names, and pushed to every stream.
// How long a burst is collapsed over. Long enough that an animating UI stops
// waking every client every frame, short enough that a person driving the
// agent does not perceive the delay -- and only ever delays a notification,
// never drops the last one.
constexpr auto kNotifyWindow = std::chrono::milliseconds(50);

void EmitToolsChanged() {
  Broadcast(R"({"jsonrpc":"2.0","method":"notifications/tools/list_changed"})");
}

void EmitResourceUpdated(const std::string& uri) {
  Broadcast(R"({"jsonrpc":"2.0","method":"notifications/resources/updated")"
            R"(,"params":{"uri":)" +
                Escape(uri) + "}}",
            uri);
}

// Sleeps out the window once something is pending, then flushes it.
//
// Idle costs nothing: this blocks until a notification actually arrives rather
// than waking every window to find nothing to do.
void NotifyLoop() {
  Transport& t = TheTransport();
  while (true) {
    bool flush_tools = false;
    std::vector<std::string> flush_resources;
    {
      std::unique_lock<std::mutex> lock(t.notify_mutex);
      t.notify_cv.wait(lock, [&t]() {
        return !t.notify_running || t.tools_pending ||
               !t.resources_pending.empty();
      });
      if (!t.notify_running) {
        // Nothing is flushed on the way out: stop() closes every stream, so
        // there is no one left to tell.
        return;
      }
      // Wait out the window with the lock released, so the burst this is
      // collapsing can keep arriving while it does.
      t.notify_cv.wait_for(lock, kNotifyWindow,
                           [&t]() { return !t.notify_running; });
      if (!t.notify_running) {
        return;
      }
      const auto now = std::chrono::steady_clock::now();
      flush_tools = t.tools_pending;
      t.tools_pending = false;
      if (flush_tools) {
        t.tools_last = now;
      }
      for (const std::string& uri : t.resources_pending) {
        flush_resources.push_back(uri);
        t.resource_last[uri] = now;
      }
      t.resources_pending.clear();
    }

    // Emitted with the lock released: Broadcast takes the streams mutex, and
    // nesting the two would put a lock order between the notification path and
    // every other thing that touches streams.
    if (flush_tools) {
      EmitToolsChanged();
    }
    for (const std::string& uri : flush_resources) {
      EmitResourceUpdated(uri);
    }
  }
}

void OnRegistryNotification(const IhsMcpNotification kind,
                            const char* uri,
                            void* /* user_data */) {
  const std::string resource = kind == IHS_MCP_NOTIFY_TOOLS_CHANGED
                                   ? std::string()
                                   : std::string(uri != nullptr ? uri : "");

  Transport& t = TheTransport();
  bool emit_now = false;
  {
    const std::lock_guard<std::mutex> lock(t.notify_mutex);
    const auto now = std::chrono::steady_clock::now();
    // Leading edge: the first change after a quiet period goes out at once, so
    // an idle UI reacting to a single action is not made to feel laggy by a
    // mechanism that exists for bursts.
    if (kind == IHS_MCP_NOTIFY_TOOLS_CHANGED) {
      if (now - t.tools_last >= kNotifyWindow) {
        t.tools_last = now;
        emit_now = true;
      } else {
        t.tools_pending = true;
      }
    } else {
      const auto last = t.resource_last.find(resource);
      if (last == t.resource_last.end() ||
          now - last->second >= kNotifyWindow) {
        t.resource_last[resource] = now;
        emit_now = true;
      } else {
        t.resources_pending.insert(resource);
      }
    }
  }
  t.notify_cv.notify_one();

  if (emit_now) {
    if (kind == IHS_MCP_NOTIFY_TOOLS_CHANGED) {
      EmitToolsChanged();
    } else {
      EmitResourceUpdated(resource);
    }
  }
}

// Turns an accepted connection into an event stream. Returns false when the
// client did not ask for one, leaving it to be served as an ordinary request.
bool TryOpenEventStream(const int fd,
                        const std::string& headers,
                        const std::string& session) {
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

  {
    Transport& t = TheTransport();
    const std::lock_guard<std::mutex> lock(t.streams_mutex);
    if (t.streams.size() >= kMaxStreams) {
      WriteAll(fd, HttpResponse(503, "Service Unavailable",
                                R"({"error":"too many open event streams"})"));
      return true;  // answered; caller should close
    }
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
  t.streams.push_back(Stream{fd, session});
  return true;
}

// Serves one connection. Returns true when the descriptor has become an event
// stream and must stay open; the caller closes it otherwise.
// Closes a connection we are refusing, without losing the reason.
//
// A close with the peer's request still sitting unread discards both
// directions, so the response we just wrote can vanish and the client sees an
// empty reply rather than the refusal. Consuming what it sent first is what
// makes the answer reliably arrive.
//
// The drain is bounded in both bytes and time: this runs before any
// authorisation has succeeded, so a peer that keeps sending must not be able
// to hold the accept thread. Whatever is left when the bound is reached is
// abandoned -- at that point the response has had every chance to land, and
// the peer is misbehaving anyway.
void CloseRefused(const int fd) {
  static constexpr size_t kMaxDrainBytes = 64 * 1024;
  static constexpr int kDrainTimeoutMs = 50;

  ::shutdown(fd, SHUT_WR);
  size_t drained = 0;
  while (drained < kMaxDrainBytes) {
    pollfd poll_fd = {fd, POLLIN, 0};
    if (::poll(&poll_fd, 1, kDrainTimeoutMs) <= 0) {
      break;
    }
    char discard[4096];
    const ssize_t got = ::recv(fd, discard, sizeof(discard), 0);
    if (got <= 0) {
      break;  // EOF, or an error that makes further reading pointless
    }
    drained += static_cast<size_t>(got);
  }
  ::close(fd);
}

bool ServeConnection(const int fd, const size_t max_body) {
  if (!PeerIsOwner(fd)) {
    Log(IHS_LEVEL_WARN, "mcp: refused a connection from another user");
    WriteAll(fd, HttpResponse(403, "Forbidden", R"({"error":"forbidden"})"));
    // Returning true means "the descriptor is handled": this closes it itself,
    // because a plain close here races the response away.
    CloseRefused(fd);
    return true;
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
    // The stream is the more valuable target of the two: it is a live feed of
    // the UI rather than a single call, and it takes this separate path.
    if (CarriesOrigin(headers)) {
      Log(IHS_LEVEL_WARN, "mcp: refused a stream carrying an Origin header");
      WriteAll(fd, HttpResponse(403, "Forbidden",
                                R"({"error":"origin not allowed"})"));
      return false;
    }
    // The stream carries the session so notifications can be filtered to
    // what it subscribed to; absent, it is an unfiltered stream.
    if (TryOpenEventStream(fd, headers,
                           HeaderValue(headers, "mcp-session-id:"))) {
      Transport& t = TheTransport();
      const std::lock_guard<std::mutex> lock(t.streams_mutex);
      // Kept open only if it actually joined the registry; a refused GET was
      // answered and is finished with.
      return std::find_if(t.streams.begin(), t.streams.end(),
                          [fd](const Stream& s) { return s.fd == fd; }) !=
             t.streams.end();
    }
    return false;
  }

  std::string body;
  std::string headers;
  if (!ReadRequest(fd, max_body, &body, &headers)) {
    return false;  // ReadRequest already answered, or the peer went away
  }

  std::string new_session;
  const std::string response =
      Dispatch(body, HeaderValue(headers, "mcp-session-id:"), &new_session);
  if (response.empty()) {
    // A notification. HTTP still needs a reply, and 202 says "taken, nothing
    // to return" without inventing a JSON-RPC response the client must ignore.
    WriteAll(fd, HttpResponse(202, "Accepted", ""));
    return false;
  }
  // initialize answers with the session it minted. In the header rather than
  // the result body, which is where the Streamable HTTP transport puts it and
  // means no invented field in a response the specification defines.
  const std::string extra = new_session.empty()
                                ? std::string()
                                : "Mcp-Session-Id: " + new_session + "\r\n";
  WriteAll(fd, HttpResponse(200, "OK", response, extra));
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

// Drops the streams that `poll` reported as hung up or readable.
//
// `polled` holds the two fixed descriptors first, so the streams begin at
// index 2. Matching back by descriptor number is safe here for a reason worth
// stating: streams are only ever added by the accept thread, which is the one
// calling this, so the set cannot have grown since the snapshot. It can have
// shrunk -- Broadcast drops a stream that will not take a frame -- and a
// descriptor that is gone is simply not found, which is why nothing is closed
// unless it was still present and removed here. Closing on the strength of the
// snapshot alone would eventually close a descriptor belonging to something
// else entirely.
void ReapClosedStreams(const std::vector<pollfd>& polled) {
  Transport& t = TheTransport();
  const std::lock_guard<std::mutex> lock(t.streams_mutex);
  for (size_t i = 2; i < polled.size(); ++i) {
    if ((polled[i].revents & (POLLHUP | POLLERR | POLLNVAL | POLLIN)) == 0) {
      continue;
    }
    const auto it =
        std::find_if(t.streams.begin(), t.streams.end(),
                     [&](const Stream& s) { return s.fd == polled[i].fd; });
    if (it == t.streams.end()) {
      continue;  // already dropped by a failed write; not ours to close
    }
    ::close(it->fd);
    t.streams.erase(it);
  }
}

void AcceptLoop() {
  Transport& t = TheTransport();
  const int listen_fd = t.listen_fd;
  const int wake_fd = t.wake_fd;
  const size_t max_body = t.max_request_bytes;

  // Reused across iterations: two fixed entries plus one per open stream.
  std::vector<pollfd> fds;
  fds.reserve(2 + kMaxStreams);

  while (true) {
    fds.clear();
    fds.push_back({listen_fd, POLLIN, 0});
    fds.push_back({wake_fd, POLLIN, 0});
    // Watch the open streams too. Nothing is ever read from them -- they carry
    // one direction -- but a client that hung up shows up as POLLHUP here, and
    // that is the only prompt notice of it there is. The alternative is to
    // discover the loss on the next write, which needs a notification that may
    // never come, so an abandoned stream would hold its descriptor until the
    // shell exits. A client looping over open-and-abandon then costs the whole
    // process its descriptor table, not just the MCP surface.
    //
    // POLLIN is included because a client that sends on a stream has also
    // given us EOF to detect: on a stream we never read, readable and hung-up
    // arrive the same way and both mean the same thing.
    {
      const std::lock_guard<std::mutex> lock(t.streams_mutex);
      for (const Stream& stream : t.streams) {
        fds.push_back({stream.fd, POLLIN, 0});
      }
    }

    const int ready = ::poll(fds.data(), fds.size(), -1);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if ((fds[1].revents & POLLIN) != 0) {
      break;  // stop() asked
    }

    // Reap before accepting, so a client at the stream cap that has already
    // gone away does not keep a live one out.
    ReapClosedStreams(fds);

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
  {
    const std::lock_guard<std::mutex> notify_lock(t.notify_mutex);
    t.notify_running = true;
  }
  t.notify_thread = std::thread(NotifyLoop);

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

  // Joined before the streams close, so no pending flush can write to a
  // descriptor this is about to shut. The registry sink was uninstalled above,
  // so nothing new can arrive while this winds down.
  std::thread notify_thread;
  {
    const std::lock_guard<std::mutex> notify_lock(t.notify_mutex);
    t.notify_running = false;
    t.tools_pending = false;
    t.resources_pending.clear();
    t.resource_last.clear();
    notify_thread = std::move(t.notify_thread);
  }
  t.notify_cv.notify_all();
  if (notify_thread.joinable()) {
    notify_thread.join();
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
  for (const Stream& stream : t.streams) {
    ::close(stream.fd);
  }
  t.streams.clear();
  // Subscriptions describe streams that no longer exist, and a restart mints
  // new session ids, so nothing here could be matched again.
  t.subscriptions.clear();
  // The sessions themselves go with them. A session that survived a restart
  // would let a client subscribe against an id from before the socket was
  // rebound, and would spend the cap on ids nothing can reach.
  t.sessions.clear();
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
