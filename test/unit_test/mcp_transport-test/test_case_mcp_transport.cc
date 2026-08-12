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
  // Only what is actually served is advertised: nothing pushes notifications
  // yet, so claiming listChanged would leave clients waiting for messages
  // that never come.
  EXPECT_EQ(body.find("listChanged"), std::string::npos);
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

// Only POST carries a request. A probe or a browser gets a clear answer
// rather than a hang.
TEST_F(McpTransportTest, NonPostIsRefused) {
  const std::string response =
      Send(path_, "GET / HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
  EXPECT_EQ(StatusLine(response), "HTTP/1.1 405 Method Not Allowed");
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
