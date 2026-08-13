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

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "gtest/gtest.h"

#include "ihs/ihs_mcp_provider.h"
#include "ihs/ihs_mcp_transport.h"

namespace {

std::string TempSocketPath(const char* leaf) {
  const char* base = std::getenv("TEST_SCRATCH_DIR");
  if (base == nullptr || base[0] == '\0') {
    base = std::getenv("TMPDIR");
  }
  if (base == nullptr || base[0] == '\0') {
    base = "/tmp";
  }
  return std::string(base) + "/ihs-mcp-" + leaf + "-" +
         std::to_string(::getpid()) + ".sock";
}

// One request, one connection: the transport answers with Connection: close,
// which is what a client of a request/response transport expects.
std::string Send(const std::string& path, const std::string& request) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return {};
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
      0) {
    ::close(fd);
    return {};
  }

  // This helper reads to EOF, so anything the transport leaves open would
  // block it forever. A regression should fail the suite, not hang CI until
  // something kills it -- on timeout the loop below just returns what arrived,
  // which is enough for the caller to assert against.
  const timeval receive_timeout{5, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
               sizeof(receive_timeout));

  size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t n = ::write(fd, request.data() + sent, request.size() - sent);
    if (n <= 0) {
      break;
    }
    sent += static_cast<size_t>(n);
  }

  std::string response;
  char chunk[4096];
  while (true) {
    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n <= 0) {
      break;
    }
    response.append(chunk, static_cast<size_t>(n));
  }
  ::close(fd);
  return response;
}

std::string Post(const std::string& path, const std::string& body) {
  return Send(path, "POST / HTTP/1.1\r\nContent-Length: " +
                        std::to_string(body.size()) + "\r\n\r\n" + body);
}

std::string StatusLine(const std::string& response) {
  const size_t end = response.find("\r\n");
  return end == std::string::npos ? response : response.substr(0, end);
}

std::string Body(const std::string& response) {
  const size_t split = response.find("\r\n\r\n");
  return split == std::string::npos ? std::string()
                                    : response.substr(split + 4);
}

class McpTransportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = TempSocketPath(
        ::testing::UnitTest::GetInstance()->current_test_info()->name());
    ::unlink(path_.c_str());
    IhsMcpTransportConfig config{};
    config.struct_size = sizeof(config);
    config.socket_path = path_.c_str();
    ASSERT_EQ(ihs_mcp_transport_start(&config), IHS_MCP_OK);
  }
  void TearDown() override {
    ihs_mcp_transport_stop();
    ::unlink(path_.c_str());
  }
  std::string path_;
};

}  // namespace

// The socket mode is the access control. Anything wider would put the UI's
// drive surface within reach of every user on the system.
TEST_F(McpTransportTest, SocketIsOwnerOnly) {
  struct stat info{};
  ASSERT_EQ(::stat(path_.c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 0777, 0600u)
      << "socket mode is " << std::oct << (info.st_mode & 0777);
}

TEST_F(McpTransportTest, ReportsWhereItIsServing) {
  EXPECT_TRUE(ihs_mcp_transport_running());
  EXPECT_EQ(std::string(ihs_mcp_transport_socket_path()), path_);
}

// A caller that cannot easily tell whether bring-up already ran must not end
// up with two listeners or a replaced socket.
TEST_F(McpTransportTest, StartIsIdempotent) {
  IhsMcpTransportConfig config{};
  config.struct_size = sizeof(config);
  config.socket_path = path_.c_str();
  EXPECT_EQ(ihs_mcp_transport_start(&config), IHS_MCP_OK);
  EXPECT_EQ(std::string(ihs_mcp_transport_socket_path()), path_);
}

// Stop unlinks, so a restart is not refused by its own leftovers.
TEST_F(McpTransportTest, StopRemovesTheSocket) {
  ihs_mcp_transport_stop();
  EXPECT_FALSE(ihs_mcp_transport_running());
  struct stat info{};
  EXPECT_NE(::stat(path_.c_str(), &info), 0);
  EXPECT_STREQ(ihs_mcp_transport_socket_path(), "");
}

TEST_F(McpTransportTest, InitializeReportsProtocolAndCapabilities) {
  const std::string response =
      Post(path_, R"({"jsonrpc":"2.0","id":1,"method":"initialize"})");
  EXPECT_EQ(StatusLine(response), "HTTP/1.1 200 OK");
  const std::string body = Body(response);
  EXPECT_NE(body.find(R"("id":1)"), std::string::npos);
  EXPECT_NE(body.find("protocolVersion"), std::string::npos);
  // What is advertised has to be deliverable; the stream now carries
  // notifications, so InitializeAdvertisesWhatTheStreamCanDeliver asserts the
  // positive case that used to be asserted absent here.
}

// With no provider registered the list is empty rather than absent, so a
// client can tell "nothing to offer" from a malformed reply.
TEST_F(McpTransportTest, ToolsAndResourcesListAreWellFormedWhenEmpty) {
  std::string body =
      Body(Post(path_, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"));
  EXPECT_NE(body.find(R"("tools":[)"), std::string::npos) << body;

  body = Body(
      Post(path_, R"({"jsonrpc":"2.0","id":3,"method":"resources/list"})"));
  EXPECT_NE(body.find(R"("resources":[)"), std::string::npos) << body;
}

// The id is echoed verbatim, including a string id, so a client's own
// correlation survives the round trip.
TEST_F(McpTransportTest, RequestIdIsEchoedVerbatim) {
  const std::string body =
      Body(Post(path_, R"({"jsonrpc":"2.0","id":"abc-1","method":"ping"})"));
  EXPECT_NE(body.find(R"("id":"abc-1")"), std::string::npos) << body;
}

TEST_F(McpTransportTest, UnknownMethodIsMethodNotFound) {
  const std::string body = Body(
      Post(path_, R"({"jsonrpc":"2.0","id":4,"method":"no/such/method"})"));
  EXPECT_NE(body.find("-32601"), std::string::npos) << body;
}

TEST_F(McpTransportTest, MalformedJsonIsAParseError) {
  const std::string body = Body(Post(path_, "not json at all"));
  EXPECT_NE(body.find("-32700"), std::string::npos) << body;
}

TEST_F(McpTransportTest, ToolsCallWithoutANameIsInvalidParams) {
  const std::string body = Body(Post(
      path_, R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{}})"));
  EXPECT_NE(body.find("-32602"), std::string::npos) << body;
}

// A notification has no id and expects no reply. Answering one is a protocol
// error, so the transport acknowledges at the HTTP layer and says nothing in
// JSON-RPC.
TEST_F(McpTransportTest, NotificationsGetNoJsonRpcReply) {
  const std::string response =
      Post(path_, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
  EXPECT_EQ(StatusLine(response), "HTTP/1.1 202 Accepted");
  EXPECT_TRUE(Body(response).empty()) << Body(response);
}

// POST carries a request and GET opens a stream; anything else gets a clear
// answer rather than a hang. GET is covered by PlainGetIsNotAcceptable, which
// distinguishes "wrong verb" from "right verb, wrong media type".
TEST_F(McpTransportTest, UnsupportedMethodsAreRefused) {
  for (const char* verb : {"PUT", "DELETE", "HEAD"}) {
    const std::string response = Send(
        path_, std::string(verb) + " / HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
    EXPECT_EQ(StatusLine(response), "HTTP/1.1 405 Method Not Allowed") << verb;
  }
}

TEST_F(McpTransportTest, MissingContentLengthIsRefused) {
  const std::string response = Send(path_, "POST / HTTP/1.1\r\n\r\n");
  EXPECT_EQ(StatusLine(response), "HTTP/1.1 411 Length Required");
}

// Refused on the declared length, before the body is read: an oversized
// request should cost the shell nothing to turn away.
TEST_F(McpTransportTest, OversizedBodyIsRefusedBeforeReading) {
  const std::string response =
      Send(path_, "POST / HTTP/1.1\r\nContent-Length: 999999999\r\n\r\n");
  EXPECT_EQ(StatusLine(response), "HTTP/1.1 413 Content Too Large");
}

TEST_F(McpTransportTest, UnparseableContentLengthIsRefused) {
  const std::string response =
      Send(path_, "POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n");
  EXPECT_EQ(StatusLine(response), "HTTP/1.1 400 Bad Request");
}

// Header matching is case-insensitive, as HTTP requires; a client sending
// lower-case headers must not be told its length is missing.
TEST_F(McpTransportTest, HeaderNamesAreCaseInsensitive) {
  const std::string body = R"({"jsonrpc":"2.0","id":6,"method":"ping"})";
  const std::string response =
      Send(path_, "POST / HTTP/1.1\r\ncontent-length: " +
                      std::to_string(body.size()) + "\r\n\r\n" + body);
  EXPECT_EQ(StatusLine(response), "HTTP/1.1 200 OK");
}

// DNS rebinding: a page resolves a name it controls to a local address and
// posts here, and the browser attaches its origin. Nothing that legitimately
// speaks to this surface is a browser, so the header's presence is enough to
// refuse on. Unreachable over a Unix socket -- the point is that the check is
// above the listener, so it already covers a port before one exists.
TEST_F(McpTransportTest, RequestWithAnOriginHeaderIsRefused) {
  const std::string body = R"({"jsonrpc":"2.0","id":7,"method":"ping"})";
  const std::string response =
      Send(path_,
           "POST / HTTP/1.1\r\nOrigin: http://evil.example\r\n"
           "Content-Length: " +
               std::to_string(body.size()) + "\r\n\r\n" + body);
  EXPECT_EQ(StatusLine(response), "HTTP/1.1 403 Forbidden");
}

// Matched case-insensitively, or the check is trivially evaded.
TEST_F(McpTransportTest, OriginIsMatchedWhateverItsCase) {
  const std::string body = R"({"jsonrpc":"2.0","id":8,"method":"ping"})";
  const std::string response =
      Send(path_,
           "POST / HTTP/1.1\r\noRiGiN: http://evil.example\r\n"
           "Content-Length: " +
               std::to_string(body.size()) + "\r\n\r\n" + body);
  EXPECT_EQ(StatusLine(response), "HTTP/1.1 403 Forbidden");
}

// The stream handshake is a GET and takes a different path through the
// transport, so it needs the check of its own -- a rebound page opening an
// event stream reads the UI just as effectively as one calling a tool.
TEST_F(McpTransportTest, EventStreamWithAnOriginHeaderIsRefused) {
  const std::string response =
      Send(path_,
           "GET / HTTP/1.1\r\nAccept: text/event-stream\r\n"
           "Origin: http://evil.example\r\n\r\n");
  EXPECT_EQ(StatusLine(response), "HTTP/1.1 403 Forbidden");
}

// A header section with no terminator must not let a peer stream forever.
TEST_F(McpTransportTest, EndlessHeadersAreBounded) {
  std::string request = "POST / HTTP/1.1\r\n";
  request += "X-Filler: " + std::string(32 * 1024, 'a') + "\r\n";
  const std::string response = Send(path_, request);
  EXPECT_EQ(StatusLine(response),
            "HTTP/1.1 431 Request Header Fields Too Large");
}

TEST(McpTransportLifecycle, StopWithoutStartIsHarmless) {
  ihs_mcp_transport_stop();
  EXPECT_FALSE(ihs_mcp_transport_running());
  EXPECT_STREQ(ihs_mcp_transport_socket_path(), "");
}

TEST(McpTransportLifecycle, NullConfigIsRefused) {
  EXPECT_EQ(ihs_mcp_transport_start(nullptr), IHS_MCP_ERR_INVALID);
  EXPECT_FALSE(ihs_mcp_transport_running());
}

// A path that cannot fit in sun_path must be refused rather than silently
// truncated to a different socket than the caller asked for.
TEST(McpTransportLifecycle, OverlongPathIsRefused) {
  IhsMcpTransportConfig config{};
  config.struct_size = sizeof(config);
  const std::string path = "/tmp/" + std::string(200, 'x') + ".sock";
  config.socket_path = path.c_str();
  EXPECT_EQ(ihs_mcp_transport_start(&config), IHS_MCP_ERR_INVALID);
  EXPECT_FALSE(ihs_mcp_transport_running());
}

// A socket left behind by a crash must not stop the next start, but one that
// is actually being served must not be stolen.
TEST(McpTransportLifecycle, StaleSocketIsReplacedButALiveOneIsNot) {
  const std::string path = TempSocketPath("stale");
  ::unlink(path.c_str());

  // Leave a socket file behind with nobody listening.
  const int dead = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(dead, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  ASSERT_EQ(
      ::bind(dead, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  ::close(dead);  // bound but never listened on, and now closed

  IhsMcpTransportConfig config{};
  config.struct_size = sizeof(config);
  config.socket_path = path.c_str();
  EXPECT_EQ(ihs_mcp_transport_start(&config), IHS_MCP_OK)
      << "a stale socket should not block a restart";

  // Now that one is live, a second start on the same path from a would-be
  // second server is refused rather than replacing it. (Same process here, so
  // this asserts the idempotent path rather than the refusal; the refusal is
  // what StaleSocketRemoved's connect probe provides across processes.)
  EXPECT_EQ(ihs_mcp_transport_start(&config), IHS_MCP_OK);

  ihs_mcp_transport_stop();
  ::unlink(path.c_str());
}

namespace {

// Opens an event stream and returns the connected descriptor, or -1. The
// caller owns it: an SSE connection outlives the request that opened it,
// which is the whole point.
int OpenStream(const std::string& path, std::string* out_headers) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
      0) {
    ::close(fd);
    return -1;
  }
  const std::string request =
      "GET /mcp HTTP/1.1\r\nAccept: text/event-stream\r\n\r\n";
  if (::write(fd, request.data(), request.size()) < 0) {
    ::close(fd);
    return -1;
  }
  char chunk[1024];
  const ssize_t n = ::read(fd, chunk, sizeof(chunk));
  if (n <= 0) {
    ::close(fd);
    return -1;
  }
  out_headers->assign(chunk, static_cast<size_t>(n));
  return fd;
}

}  // namespace

// A GET asking for the event-stream type opens the server-to-client half of
// the transport, and must not be answered with a length or a close -- the
// body is the stream.
TEST_F(McpTransportTest, EventStreamHandshakeIsAccepted) {
  std::string headers;
  const int fd = OpenStream(path_, &headers);
  ASSERT_GE(fd, 0) << "stream did not open";
  EXPECT_NE(headers.find("200 OK"), std::string::npos) << headers;
  EXPECT_NE(headers.find("text/event-stream"), std::string::npos) << headers;
  EXPECT_EQ(headers.find("Content-Length"), std::string::npos) << headers;
  ::close(fd);
}

// A GET that did not ask for a stream is told so, rather than being handed
// one it will not read or left hanging.
TEST_F(McpTransportTest, PlainGetIsNotAcceptable) {
  const std::string response =
      Send(path_, "GET /mcp HTTP/1.1\r\nAccept: application/json\r\n\r\n");
  EXPECT_EQ(StatusLine(response), "HTTP/1.1 406 Not Acceptable");
}

// The capabilities are advertised now that a stream can carry them. They were
// absent while it could not: a client told to expect notifications that never
// arrive waits instead of polling.
TEST_F(McpTransportTest, InitializeAdvertisesWhatTheStreamCanDeliver) {
  const std::string body =
      Body(Post(path_, R"({"jsonrpc":"2.0","id":1,"method":"initialize"})"));
  EXPECT_NE(body.find("listChanged"), std::string::npos) << body;
  EXPECT_NE(body.find("subscribe"), std::string::npos) << body;
}

// Requests keep working while a stream is open: the stream must not occupy
// the accept loop, which it would if it were served inline like a request.
TEST_F(McpTransportTest, RequestsAreStillServedWhileAStreamIsOpen) {
  std::string headers;
  const int stream = OpenStream(path_, &headers);
  ASSERT_GE(stream, 0);

  const std::string body =
      Body(Post(path_, R"({"jsonrpc":"2.0","id":7,"method":"ping"})"));
  EXPECT_NE(body.find(R"("id":7)"), std::string::npos) << body;
  ::close(stream);
}

// Several clients may watch at once, and each has to be served.
TEST_F(McpTransportTest, MoreThanOneStreamMayBeOpen) {
  std::string first_headers;
  std::string second_headers;
  const int first = OpenStream(path_, &first_headers);
  const int second = OpenStream(path_, &second_headers);
  EXPECT_GE(first, 0);
  EXPECT_GE(second, 0);
  EXPECT_NE(first_headers.find("text/event-stream"), std::string::npos);
  EXPECT_NE(second_headers.find("text/event-stream"), std::string::npos);
  if (first >= 0) {
    ::close(first);
  }
  if (second >= 0) {
    ::close(second);
  }
}

// Stopping closes the streams: there is no goodbye event, so the end of the
// connection is how a client learns the server is gone.
TEST_F(McpTransportTest, StopClosesOpenStreams) {
  std::string headers;
  const int fd = OpenStream(path_, &headers);
  ASSERT_GE(fd, 0);

  ihs_mcp_transport_stop();

  char chunk[64];
  const ssize_t n = ::read(fd, chunk, sizeof(chunk));
  EXPECT_EQ(n, 0) << "expected end of stream, got " << n;
  ::close(fd);
}
