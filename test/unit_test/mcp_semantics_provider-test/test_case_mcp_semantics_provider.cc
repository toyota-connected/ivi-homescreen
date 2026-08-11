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

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "ihs/ihs_mcp_host.h"
#include "ihs/ihs_mcp_provider.h"
#include "ihs/ihs_mcp_semantics.h"
#include "ihs/ihs_semantics.h"
#include "ihs/ihs_semantics_host.h"

namespace {

// Records what the provider asked the shell to do, so a test can assert the
// dispatch reached the far side with the right node and action rather than
// only that the call returned OK.
struct MockHost {
  int dispatch_calls = 0;
  int32_t last_node_id = 0;
  uint64_t last_action = 0;
  bool semantics_enabled = false;
};

MockHost* g_host = nullptr;

void OnEnable(void* /*user_data*/, bool enabled) {
  g_host->semantics_enabled = enabled;
}

int OnDispatch(void* /*user_data*/,
               int64_t /*view_id*/,
               int32_t node_id,
               uint64_t action,
               const uint8_t* /*data*/,
               size_t /*data_length*/) {
  g_host->dispatch_calls++;
  g_host->last_node_id = node_id;
  g_host->last_action = action;
  return IHS_SEMANTICS_OK;
}

// Builds and publishes a tree, owning the arrays the info points at.
struct TreeBuilder {
  std::vector<IhsSemanticsPublishNode> nodes;
  std::vector<std::vector<int32_t>> children;

  IhsSemanticsPublishNode& Add(int32_t id,
                               const char* label,
                               IhsSemanticsRole role) {
    children.emplace_back();
    IhsSemanticsPublishNode node{};
    node.id = id;
    node.label = label;
    node.role = role;
    node.enabled = IHS_SEMANTICS_TRISTATE_NONE;
    nodes.push_back(node);
    return nodes.back();
  }

  int Publish() {
    IhsSemanticsPublishInfo info{};
    info.struct_size = sizeof(IhsSemanticsPublishInfo);
    info.nodes = nodes.data();
    info.node_count = nodes.size();
    return ihs_semantics_publish(&info);
  }
};

std::string CallTool(const char* name, const char* args, int* status_out) {
  IhsMcpPayload payload{};
  const int status =
      ihs_mcp_host_call_tool(name, args, args ? strlen(args) : 0, &payload);
  if (status_out != nullptr) {
    *status_out = status;
  }
  std::string body;
  if (payload.data != nullptr) {
    body.assign(payload.data, payload.length);
  }
  ihs_mcp_host_release_payload(&payload);
  return body;
}

bool Contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

class McpSemanticsProviderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    host_ = MockHost{};
    g_host = &host_;
    IhsSemanticsHost host{};
    host.struct_size = sizeof(IhsSemanticsHost);
    host.set_semantics_enabled = OnEnable;
    host.dispatch = OnDispatch;
    ihs_semantics_set_host(&host);
    ihs_semantics_clear();
    ASSERT_EQ(ihs_mcp_semantics_provider_start(), IHS_MCP_OK);
  }

  void TearDown() override {
    ihs_mcp_semantics_provider_stop();
    ihs_semantics_clear();
    ihs_semantics_set_host(nullptr);
    g_host = nullptr;
    ASSERT_EQ(ihs_mcp_provider_count(), 0u);
  }

  MockHost host_;
};

}  // namespace

// Registering the provider is what asks the engine for semantics, so nothing
// is produced until something actually wants it.
TEST_F(McpSemanticsProviderTest, StartingTheProviderTurnsSemanticsOn) {
  EXPECT_TRUE(host_.semantics_enabled);
  EXPECT_TRUE(ihs_mcp_semantics_provider_running());
}

TEST_F(McpSemanticsProviderTest, StartIsIdempotent) {
  EXPECT_EQ(ihs_mcp_semantics_provider_start(), IHS_MCP_OK);
  EXPECT_EQ(ihs_mcp_provider_count(), 1u);
}

// Tools reach a client under the provider's prefix, so a call carries the
// prefixed name and the provider still sees its own vocabulary.
TEST_F(McpSemanticsProviderTest, ToolsAreAdvertisedUnderTheUiPrefix) {
  std::vector<std::string> names;
  ihs_mcp_host_for_each_tool(
      [](const IhsMcpHostTool* tool, void* user_data) {
        static_cast<std::vector<std::string>*>(user_data)->emplace_back(
            tool->name);
      },
      &names);
  ASSERT_FALSE(names.empty());
  for (const std::string& name : names) {
    EXPECT_EQ(name.rfind("ui_", 0), 0u) << name << " escaped the prefix";
  }
  EXPECT_NE(std::find(names.begin(), names.end(), "ui_tap"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "ui_snapshot"), names.end());
}

// The accessibility-focus actions must not be offered: an agent moving the
// screen-reader cursor is exactly what the allow mask exists to prevent.
TEST_F(McpSemanticsProviderTest, NoToolExposesAccessibilityFocus) {
  std::vector<std::string> names;
  ihs_mcp_host_for_each_tool(
      [](const IhsMcpHostTool* tool, void* user_data) {
        static_cast<std::vector<std::string>*>(user_data)->emplace_back(
            tool->name);
      },
      &names);
  for (const std::string& name : names) {
    EXPECT_EQ(name.find("focus"), std::string::npos)
        << name << " would let a client move the accessibility cursor";
  }
}

TEST_F(McpSemanticsProviderTest, SnapshotSerializesThePublishedTree) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& button =
      tree.Add(7, "Play", IHS_SEMANTICS_ROLE_BUTTON);
  button.actions = IHS_SEMANTICS_ACTION_TAP;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  const std::string body = CallTool("ui_snapshot", nullptr, &status);
  EXPECT_EQ(status, IHS_MCP_OK);
  EXPECT_TRUE(Contains(body, "\"generation\""));
  EXPECT_TRUE(Contains(body, "\"label\":\"Play\""));
  EXPECT_TRUE(Contains(body, "\"role\":\"button\""));
  // Verbs are named as the tools that carry them, so a client reads the
  // answer and calls it without a second mapping.
  EXPECT_TRUE(Contains(body, "\"actions\":[\"tap\"]"));
}

// A password field's contents are not something to hand a client.
TEST_F(McpSemanticsProviderTest, ObscuredValuesAreNotSerialized) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& secret =
      tree.Add(3, "Password", IHS_SEMANTICS_ROLE_PASSWORD_INPUT);
  secret.value = "hunter2";
  secret.obscured = true;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  const std::string body = CallTool("ui_snapshot", nullptr, nullptr);
  EXPECT_TRUE(Contains(body, "password_input"));
  EXPECT_FALSE(Contains(body, "hunter2"));
}

// Tristates cross the wire as three values: a client reading "false" must be
// able to tell a disabled control from one where enablement is meaningless.
TEST_F(McpSemanticsProviderTest, TristatesAreNotFlattened) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& disabled =
      tree.Add(1, "Save", IHS_SEMANTICS_ROLE_BUTTON);
  disabled.enabled = IHS_SEMANTICS_TRISTATE_FALSE;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  const std::string body = CallTool("ui_snapshot", nullptr, nullptr);
  EXPECT_TRUE(Contains(body, "\"enabled\":\"false\""));
  EXPECT_TRUE(Contains(body, "\"enabled\":\"not_applicable\""));
}

TEST_F(McpSemanticsProviderTest, QueryNarrowsByEverySelectorGiven) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  tree.Add(1, "Play", IHS_SEMANTICS_ROLE_BUTTON);
  tree.Add(2, "Play", IHS_SEMANTICS_ROLE_SLIDER);
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  // Label alone matches both.
  const std::string by_label =
      CallTool("ui_query", R"({"label":"Play"})", nullptr);
  EXPECT_TRUE(Contains(by_label, "\"id\":1"));
  EXPECT_TRUE(Contains(by_label, "\"id\":2"));

  // Adding a role narrows rather than widens.
  const std::string by_both =
      CallTool("ui_query", R"({"label":"Play","role":"slider"})", nullptr);
  EXPECT_FALSE(Contains(by_both, "\"id\":1"));
  EXPECT_TRUE(Contains(by_both, "\"id\":2"));
}

TEST_F(McpSemanticsProviderTest, TapDispatchesToTheShell) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& button =
      tree.Add(12, "Play", IHS_SEMANTICS_ROLE_BUTTON);
  button.actions = IHS_SEMANTICS_ACTION_TAP;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  const std::string body = CallTool("ui_tap", R"({"node_id":12})", &status);
  EXPECT_EQ(status, IHS_MCP_OK);
  EXPECT_EQ(host_.dispatch_calls, 1);
  EXPECT_EQ(host_.last_node_id, 12);
  EXPECT_EQ(host_.last_action, IHS_SEMANTICS_ACTION_TAP);
  EXPECT_TRUE(Contains(body, "\"dispatched\":true"));
}

// The framework drops an action on a disabled node silently, so a client would
// otherwise see success and no effect.
TEST_F(McpSemanticsProviderTest, DisabledNodeIsRefusedBeforeDispatch) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& button =
      tree.Add(5, "Save", IHS_SEMANTICS_ROLE_BUTTON);
  button.actions = IHS_SEMANTICS_ACTION_TAP;
  button.enabled = IHS_SEMANTICS_TRISTATE_FALSE;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  const std::string body = CallTool("ui_tap", R"({"node_id":5})", &status);
  EXPECT_EQ(status, IHS_MCP_ERR_REFUSED);
  EXPECT_EQ(host_.dispatch_calls, 0);
  EXPECT_TRUE(Contains(body, "disabled"));
}

// Same reasoning for an action the node never offered.
TEST_F(McpSemanticsProviderTest, ActionNotOfferedIsRefusedBeforeDispatch) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& node =
      tree.Add(9, "Label", IHS_SEMANTICS_ROLE_LABEL);
  node.actions = 0;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  const std::string body = CallTool("ui_tap", R"({"node_id":9})", &status);
  EXPECT_EQ(status, IHS_MCP_ERR_REFUSED);
  EXPECT_EQ(host_.dispatch_calls, 0);
  EXPECT_TRUE(Contains(body, "does not offer"));
}

TEST_F(McpSemanticsProviderTest, UnknownNodeIsReportedNotFound) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  const std::string body = CallTool("ui_tap", R"({"node_id":999})", &status);
  EXPECT_EQ(status, IHS_MCP_ERR_NOT_FOUND);
  EXPECT_EQ(host_.dispatch_calls, 0);
  EXPECT_TRUE(Contains(body, "no node matched"));
}

// Addressing by the application's identifier is the stable route where the
// engine supplies one.
TEST_F(McpSemanticsProviderTest, NodesResolveByIdentifier) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& slider =
      tree.Add(4, "Volume", IHS_SEMANTICS_ROLE_SLIDER);
  slider.identifier = "hvac.temp.driver";
  slider.actions = IHS_SEMANTICS_ACTION_INCREASE;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  CallTool("ui_increase", R"({"identifier":"hvac.temp.driver"})", &status);
  EXPECT_EQ(status, IHS_MCP_OK);
  EXPECT_EQ(host_.last_node_id, 4);
  EXPECT_EQ(host_.last_action, IHS_SEMANTICS_ACTION_INCREASE);
}

TEST_F(McpSemanticsProviderTest, ResourceReturnsTheTree) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  IhsMcpPayload payload{};
  ASSERT_EQ(ihs_mcp_host_read_resource("ui://semantics/tree", &payload),
            IHS_MCP_OK);
  const std::string body(payload.data, payload.length);
  EXPECT_TRUE(Contains(body, "\"nodes\""));
  ihs_mcp_host_release_payload(&payload);

  EXPECT_EQ(ihs_mcp_host_read_resource("ui://nope", &payload),
            IHS_MCP_ERR_NOT_FOUND);
}

// Before the engine has published anything there is no tree. Saying so beats
// an empty document that reads as "the UI has no nodes".
TEST_F(McpSemanticsProviderTest, ToolsRefuseBeforeAnythingIsPublished) {
  int status = 0;
  const std::string body = CallTool("ui_tap", R"({"node_id":1})", &status);
  EXPECT_EQ(status, IHS_MCP_ERR_REFUSED);
  EXPECT_TRUE(Contains(body, "no semantics tree"));
}

// Stopping releases the hub registration, which is what lets semantics go
// quiet again when nothing is listening.
TEST_F(McpSemanticsProviderTest, StoppingReleasesTheHubRegistration) {
  EXPECT_TRUE(host_.semantics_enabled);
  ihs_mcp_semantics_provider_stop();
  EXPECT_FALSE(host_.semantics_enabled);
  EXPECT_FALSE(ihs_mcp_semantics_provider_running());
  EXPECT_EQ(ihs_semantics_consumer_count(), 0u);
}
