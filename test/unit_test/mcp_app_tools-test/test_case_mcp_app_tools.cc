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

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "ihs/ihs_mcp_app_tools.h"
#include "ihs/ihs_mcp_provider.h"
#include "ihs/ihs_mcp_registry.h"

namespace {

// Stands in for the application. Records what it was asked and answers
// however the test tells it to -- which is the whole surface, since an
// application's only obligations are to receive an invoke and to complete it.
struct MockApp {
  std::string last_tool;
  std::string last_arguments;
  uint64_t last_call_id = 0;
  int invocations = 0;

  // How to answer. Answering from inside the invoke is the simplest thing an
  // application can do and the easiest to deadlock, so it is the default here.
  bool answer_inline = true;
  bool answer_ok = true;
  std::string answer_result = R"({"ok":true})";
};

MockApp* g_app = nullptr;

void OnInvoke(void* /*user_data*/,
              const uint64_t call_id,
              const char* tool_name,
              const char* arguments_json,
              const size_t arguments_length) {
  g_app->invocations++;
  g_app->last_tool = tool_name != nullptr ? tool_name : "";
  g_app->last_arguments.assign(arguments_json, arguments_length);
  g_app->last_call_id = call_id;
  if (g_app->answer_inline) {
    ihs_mcp_app_tools_complete(call_id, g_app->answer_ok,
                               g_app->answer_result.data(),
                               g_app->answer_result.size());
  }
}

IhsMcpAppTool Tool(const char* name, const char* schema) {
  IhsMcpAppTool tool{};
  tool.name = name;
  tool.description = "declared by the application";
  tool.input_schema_json = schema;
  tool.capability = 0;  // defaults to interact
  return tool;
}

constexpr char kTempSchema[] =
    R"({"type":"object","properties":{)"
    R"("zone":{"type":"string"},"celsius":{"type":"number"}},)"
    R"("required":["zone","celsius"]})";

std::string CallTool(const char* name, const char* args, int* status_out) {
  IhsMcpPayload payload{};
  const int status = ihs_mcp_registry_call_tool(
      name, args, args != nullptr ? std::strlen(args) : 0, &payload);
  if (status_out != nullptr) {
    *status_out = status;
  }
  std::string body;
  if (payload.data != nullptr) {
    body.assign(payload.data, payload.length);
  }
  ihs_mcp_registry_release_payload(&payload);
  return body;
}

bool Contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

class McpAppToolsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    app_ = MockApp{};
    g_app = &app_;
  }

  void TearDown() override {
    if (handle_ != nullptr) {
      ihs_mcp_app_tools_unregister(handle_);
      handle_ = nullptr;
    }
    g_app = nullptr;
    ASSERT_EQ(ihs_mcp_provider_count(), 0u);
  }

  int RegisterOne(const char* prefix = "hvac_", uint32_t timeout_ms = 0) {
    tools_ = {Tool("set_temp", kTempSchema)};
    IhsMcpAppToolsDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.tool_prefix = prefix;
    desc.tools = tools_.data();
    desc.tool_count = tools_.size();
    desc.invoke = OnInvoke;
    desc.call_timeout_ms = timeout_ms;
    return ihs_mcp_app_tools_register(&desc, &handle_);
  }

  MockApp app_;
  std::vector<IhsMcpAppTool> tools_;
  IhsMcpAppTools* handle_ = nullptr;
};

}  // namespace

// The tools reach clients under the application's own prefix, schema intact.
// The schema is the reason this exists at all: a generic tap cannot say that
// an argument is a number in a named range.
TEST_F(McpAppToolsTest, DeclaredToolsAreAdvertisedWithTheirSchema) {
  ASSERT_EQ(RegisterOne(), IHS_MCP_OK);

  struct Seen {
    bool found = false;
    std::string schema;
  } seen;
  ihs_mcp_registry_for_each_tool(
      [](const IhsMcpRegistryTool* tool, void* user_data) {
        auto* out = static_cast<Seen*>(user_data);
        if (std::strcmp(tool->name, "hvac_set_temp") == 0) {
          out->found = true;
          out->schema =
              tool->input_schema_json != nullptr ? tool->input_schema_json : "";
        }
      },
      &seen);

  EXPECT_TRUE(seen.found) << "the application's tool was not advertised";
  EXPECT_NE(seen.schema.find("celsius"), std::string::npos) << seen.schema;
}

// The round trip: a client calls, the application is handed the arguments as
// sent, and its answer comes back as the tool's result.
TEST_F(McpAppToolsTest, CallReachesTheApplicationAndItsAnswerComesBack) {
  ASSERT_EQ(RegisterOne(), IHS_MCP_OK);
  app_.answer_result = R"({"zone":"front","celsius":21.5})";

  int status = 0;
  const std::string body =
      CallTool("hvac_set_temp", R"({"zone":"front","celsius":21.5})", &status);

  EXPECT_EQ(status, IHS_MCP_OK) << body;
  EXPECT_EQ(app_.invocations, 1);
  EXPECT_EQ(app_.last_tool, "set_temp") << "the prefix is the host's, not the "
                                           "application's to strip";
  EXPECT_TRUE(Contains(app_.last_arguments, "\"celsius\":21.5"))
      << app_.last_arguments;
  EXPECT_TRUE(Contains(body, "front")) << body;
}

// A tool that ran and failed is not the same as a tool that does not exist,
// and a client that cannot tell them apart retries the wrong one.
TEST_F(McpAppToolsTest, AnApplicationFailureIsDistinctFromAMissingTool) {
  ASSERT_EQ(RegisterOne(), IHS_MCP_OK);
  app_.answer_ok = false;
  app_.answer_result = R"({"error":"zone is closed"})";

  int failed = 0;
  const std::string body =
      CallTool("hvac_set_temp", R"({"zone":"rear"})", &failed);
  EXPECT_EQ(failed, IHS_MCP_ERR_REFUSED) << body;
  EXPECT_TRUE(Contains(body, "zone is closed")) << body;

  int missing = 0;
  CallTool("hvac_no_such_tool", "{}", &missing);
  EXPECT_EQ(missing, IHS_MCP_ERR_NOT_FOUND);
}

// An application free to answer later is the point of the split between
// invoke and complete: it can hop to the UI thread and reply from there.
TEST_F(McpAppToolsTest, AnAnswerMayArriveFromAnotherThreadLater) {
  ASSERT_EQ(RegisterOne(), IHS_MCP_OK);
  app_.answer_inline = false;

  std::thread responder;
  std::atomic<bool> launched{false};
  app_.answer_result = R"({"late":true})";

  // Started from the invoke so it cannot answer before the call exists.
  struct Launcher {
    static void Go(std::thread* thread, std::atomic<bool>* launched) {
      *thread = std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::string result = R"({"late":true})";
        ihs_mcp_app_tools_complete(g_app->last_call_id, true, result.data(),
                                   result.size());
      });
      launched->store(true);
    }
  };

  // The mock records the id before this runs, so the thread has something to
  // complete.
  app_.answer_inline = false;
  int status = 0;
  std::thread starter([&]() {
    while (g_app->last_call_id == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Launcher::Go(&responder, &launched);
  });

  const std::string body =
      CallTool("hvac_set_temp", R"({"zone":"front"})", &status);
  starter.join();
  if (responder.joinable()) {
    responder.join();
  }

  EXPECT_EQ(status, IHS_MCP_OK) << body;
  EXPECT_TRUE(Contains(body, "late")) << body;
}

// An application that never answers must not hold the caller open. An agent
// blocked forever cannot tell that from one still working.
TEST_F(McpAppToolsTest, ACallThatIsNeverAnsweredTimesOut) {
  ASSERT_EQ(RegisterOne("hvac_", /*timeout_ms=*/120), IHS_MCP_OK);
  app_.answer_inline = false;

  const auto started = std::chrono::steady_clock::now();
  int status = 0;
  CallTool("hvac_set_temp", R"({"zone":"front"})", &status);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_EQ(status, IHS_MCP_ERR_UNAVAILABLE);
  EXPECT_GE(elapsed, std::chrono::milliseconds(100));
  EXPECT_LT(elapsed, std::chrono::seconds(3)) << "waited far past the timeout";
}

// A reply that arrives after the timeout is reported rather than written into
// a call nobody is waiting on.
TEST_F(McpAppToolsTest, ALateAnswerIsRefusedRatherThanIgnored) {
  ASSERT_EQ(RegisterOne("hvac_", /*timeout_ms=*/60), IHS_MCP_OK);
  app_.answer_inline = false;

  int status = 0;
  CallTool("hvac_set_temp", R"({"zone":"front"})", &status);
  ASSERT_EQ(status, IHS_MCP_ERR_UNAVAILABLE);

  const std::string late = R"({"too":"late"})";
  EXPECT_EQ(ihs_mcp_app_tools_complete(app_.last_call_id, true, late.data(),
                                       late.size()),
            IHS_MCP_ERR_NOT_FOUND);
}

// Answering twice is the application getting its own bookkeeping wrong, and
// silently accepting the second would make that impossible to notice.
TEST_F(McpAppToolsTest, AnsweringTwiceIsRefused) {
  ASSERT_EQ(RegisterOne(), IHS_MCP_OK);

  int status = 0;
  CallTool("hvac_set_temp", R"({"zone":"front"})", &status);
  ASSERT_EQ(status, IHS_MCP_OK);
  EXPECT_EQ(ihs_mcp_app_tools_complete(app_.last_call_id, true, nullptr, 0),
            IHS_MCP_ERR_NOT_FOUND);
}

// The prefix is checked against every other provider, so an application
// cannot shadow the built-in verbs or another application's tools.
TEST_F(McpAppToolsTest, ACollidingPrefixIsRefused) {
  ASSERT_EQ(RegisterOne("hvac_"), IHS_MCP_OK);

  std::vector<IhsMcpAppTool> more = {Tool("other", kTempSchema)};
  IhsMcpAppToolsDesc desc{};
  desc.struct_size = sizeof(desc);
  desc.tool_prefix = "hvac_";
  desc.tools = more.data();
  desc.tool_count = more.size();
  desc.invoke = OnInvoke;
  IhsMcpAppTools* second = nullptr;
  // Kept distinct from a plain refusal: a caller can tell "another
  // application owns this namespace" from "the host declined".
  EXPECT_EQ(ihs_mcp_app_tools_register(&desc, &second),
            IHS_MCP_ERR_PREFIX_TAKEN);
  EXPECT_EQ(second, nullptr);
}

// Unregistering has to leave nothing behind: the tools go, and a call already
// waiting is failed rather than left to time out against a callback that no
// longer exists.
TEST_F(McpAppToolsTest, UnregisterRemovesTheToolsAndFreesWaiters) {
  ASSERT_EQ(RegisterOne(), IHS_MCP_OK);
  ihs_mcp_app_tools_unregister(handle_);
  handle_ = nullptr;

  int status = 0;
  CallTool("hvac_set_temp", "{}", &status);
  EXPECT_EQ(status, IHS_MCP_ERR_NOT_FOUND);
  EXPECT_EQ(app_.invocations, 0);
}

TEST_F(McpAppToolsTest, AMalformedRegistrationIsRefused) {
  IhsMcpAppTools* handle = nullptr;
  EXPECT_EQ(ihs_mcp_app_tools_register(nullptr, &handle), IHS_MCP_ERR_INVALID);

  IhsMcpAppToolsDesc desc{};
  desc.struct_size = sizeof(desc);
  desc.tool_prefix = "x_";
  desc.invoke = nullptr;  // nothing to call
  EXPECT_EQ(ihs_mcp_app_tools_register(&desc, &handle), IHS_MCP_ERR_INVALID);

  desc.invoke = OnInvoke;
  desc.tool_prefix = "";  // no namespace to claim
  EXPECT_EQ(ihs_mcp_app_tools_register(&desc, &handle), IHS_MCP_ERR_INVALID);
}
