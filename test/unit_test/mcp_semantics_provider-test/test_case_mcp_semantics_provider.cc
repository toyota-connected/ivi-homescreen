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

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include "gtest/gtest.h"

#include <functional>
#include "ihs/ihs_mcp_provider.h"
#include "ihs/ihs_mcp_registry.h"
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
  // The argument as the hub carried it, so a test can check the plain layout
  // the shell will encode from.
  std::vector<uint8_t> last_data;
  bool semantics_enabled = false;
  int pointer_taps = 0;

  // Called from inside the dispatch, so a test can republish the tree the way
  // the framework would after handling the action. That is the case
  // verify-after-act exists for: without it every action looks like one that
  // changed nothing.
  std::function<void()> on_dispatch;
};

MockHost* g_host = nullptr;

void OnEnable(void* /*user_data*/, bool enabled) {
  g_host->semantics_enabled = enabled;
}

int OnDispatch(void* /*user_data*/,
               int64_t /*view_id*/,
               int32_t node_id,
               uint64_t action,
               const uint8_t* data,
               size_t data_length) {
  g_host->dispatch_calls++;
  g_host->last_node_id = node_id;
  g_host->last_action = action;
  g_host->last_data.assign(data, data + (data == nullptr ? 0 : data_length));
  if (g_host->on_dispatch) {
    g_host->on_dispatch();
  }
  return IHS_SEMANTICS_OK;
}

int OnPointerTap(void* /*user_data*/,
                 int64_t /*view_id*/,
                 double /*x*/,
                 double /*y*/) {
  g_host->pointer_taps++;
  return IHS_SEMANTICS_OK;
}

// Builds and publishes a tree, owning the arrays the info points at.
struct TreeBuilder {
  std::vector<IhsSemanticsPublishNode> nodes;
  std::vector<std::vector<int32_t>> children;
  std::vector<IhsSemanticsPublishCustomAction> custom_actions;
  // Stable storage: a node points at its id list, so it must outlive Publish.
  std::vector<std::vector<int32_t>> custom_ids;

  // Declares a custom action and attaches it to the node most recently added.
  void AddCustomAction(int32_t id, const char* label) {
    custom_actions.push_back(IhsSemanticsPublishCustomAction{id, label, ""});
    custom_ids.emplace_back();
  }

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
    info.custom_actions =
        custom_actions.empty() ? nullptr : custom_actions.data();
    info.custom_action_count = custom_actions.size();
    return ihs_semantics_publish(&info);
  }
};

std::string CallTool(const char* name, const char* args, int* status_out) {
  IhsMcpPayload payload{};
  const int status =
      ihs_mcp_registry_call_tool(name, args, args ? strlen(args) : 0, &payload);
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

class McpSemanticsProviderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    host_ = MockHost{};
    g_host = &host_;
    IhsSemanticsHost host{};
    host.struct_size = sizeof(IhsSemanticsHost);
    host.set_semantics_enabled = OnEnable;
    host.dispatch = OnDispatch;
    host.send_pointer_tap = OnPointerTap;
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
  ihs_mcp_registry_for_each_tool(
      [](const IhsMcpRegistryTool* tool, void* user_data) {
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
  ihs_mcp_registry_for_each_tool(
      [](const IhsMcpRegistryTool* tool, void* user_data) {
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
  ASSERT_EQ(ihs_mcp_registry_read_resource("ui://semantics/tree", &payload),
            IHS_MCP_OK);
  const std::string body(payload.data, payload.length);
  EXPECT_TRUE(Contains(body, "\"nodes\""));
  ihs_mcp_registry_release_payload(&payload);

  EXPECT_EQ(ihs_mcp_registry_read_resource("ui://nope", &payload),
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

// set_text carries the replacement text in the hub's plain layout: the bytes
// are the text, and the length is its size.
TEST_F(McpSemanticsProviderTest, SetTextCarriesTheTextAsBytes) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& field =
      tree.Add(1, "Destination", IHS_SEMANTICS_ROLE_TEXT_INPUT);
  field.actions = IHS_SEMANTICS_ACTION_SET_TEXT;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  CallTool("ui_set_text", R"({"node_id":1,"text":"Fahrertur"})", &status);
  EXPECT_EQ(status, IHS_MCP_OK);
  EXPECT_EQ(g_host->last_action, IHS_SEMANTICS_ACTION_SET_TEXT);
  EXPECT_EQ(std::string(g_host->last_data.begin(), g_host->last_data.end()),
            "Fahrertur");
}

// Text is parsed as JSON rather than scanned for, so an escaped quote inside
// the value does not truncate it. Scanning for the next quoted run -- which is
// how the identifier extractor works -- would stop at the backslash-quote.
TEST_F(McpSemanticsProviderTest, SetTextSurvivesEscapesInTheValue) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& field =
      tree.Add(1, "Destination", IHS_SEMANTICS_ROLE_TEXT_INPUT);
  field.actions = IHS_SEMANTICS_ACTION_SET_TEXT;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  CallTool("ui_set_text", R"({"node_id":1,"text":"He said \"go\" now"})",
           nullptr);
  EXPECT_EQ(std::string(g_host->last_data.begin(), g_host->last_data.end()),
            "He said \"go\" now");
}

// An agent that can type into a password field can also read back what it
// wrote through the application's own behaviour, so the field is refused.
TEST_F(McpSemanticsProviderTest, SetTextIsRefusedOnAnObscuredField) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& field =
      tree.Add(1, "Password", IHS_SEMANTICS_ROLE_PASSWORD_INPUT);
  field.actions = IHS_SEMANTICS_ACTION_SET_TEXT;
  field.obscured = true;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  const std::string body =
      CallTool("ui_set_text", R"({"node_id":1,"text":"hunter2"})", &status);
  EXPECT_EQ(status, IHS_MCP_ERR_REFUSED);
  EXPECT_EQ(g_host->dispatch_calls, 0);
  EXPECT_NE(body.find("obscured"), std::string::npos) << body;
}

TEST_F(McpSemanticsProviderTest, SetTextWithoutTextIsRefused) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& field =
      tree.Add(1, "Destination", IHS_SEMANTICS_ROLE_TEXT_INPUT);
  field.actions = IHS_SEMANTICS_ACTION_SET_TEXT;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  CallTool("ui_set_text", R"({"node_id":1})", &status);
  EXPECT_EQ(status, IHS_MCP_ERR_INVALID);
  EXPECT_EQ(g_host->dispatch_calls, 0);
}

// scroll_to carries two doubles, dx then dy. An axis the caller did not name
// is zero, so scrolling a vertical list needs only dy.
TEST_F(McpSemanticsProviderTest, ScrollToCarriesBothAxesAndDefaultsTheAbsent) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& list =
      tree.Add(1, "Media list", IHS_SEMANTICS_ROLE_SCROLL_VIEW);
  list.actions = IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  CallTool("ui_scroll_to", R"({"node_id":1,"dy":240.5})", &status);
  EXPECT_EQ(status, IHS_MCP_OK);
  EXPECT_EQ(g_host->last_action, IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET);
  ASSERT_EQ(g_host->last_data.size(), sizeof(double) * 2);
  double offset[2] = {-1.0, -1.0};
  std::memcpy(offset, g_host->last_data.data(), sizeof(offset));
  EXPECT_DOUBLE_EQ(offset[0], 0.0);
  EXPECT_DOUBLE_EQ(offset[1], 240.5);
}

// A node that does not offer the action is refused before dispatch, as with
// every other action tool.
TEST_F(McpSemanticsProviderTest, ParameterizedToolsRespectTheNodeActions) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  tree.Add(1, "Label", IHS_SEMANTICS_ROLE_LABEL);  // offers nothing
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  CallTool("ui_set_text", R"({"node_id":1,"text":"x"})", &status);
  EXPECT_EQ(status, IHS_MCP_ERR_REFUSED);
  CallTool("ui_scroll_to", R"({"node_id":1,"dy":1})", &status);
  EXPECT_EQ(status, IHS_MCP_ERR_REFUSED);
  EXPECT_EQ(g_host->dispatch_calls, 0);
}

namespace {

struct NotifyRecord {
  std::mutex mutex;
  std::condition_variable arrived;
  int updates = 0;
  std::string last_uri;
};

NotifyRecord* g_notify = nullptr;

void OnProviderNotify(IhsMcpNotification kind, const char* uri, void*) {
  if (kind != IHS_MCP_NOTIFY_RESOURCE_UPDATED) {
    return;
  }
  const std::lock_guard<std::mutex> lock(g_notify->mutex);
  g_notify->updates++;
  g_notify->last_uri = uri != nullptr ? uri : "";
  g_notify->arrived.notify_all();
}

}  // namespace

// Publishing a tree must reach a server as a resource notification, or a
// client has no way to learn the UI changed short of polling ui_snapshot.
// One descriptor carries this: the hub signals it on publish and the MCP
// registry is watching the same one.
TEST_F(McpSemanticsProviderTest, PublishingATreeNotifiesTheServer) {
  NotifyRecord record;
  g_notify = &record;
  ASSERT_EQ(ihs_mcp_registry_set_notification_sink(OnProviderNotify, nullptr),
            IHS_MCP_OK);

  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  {
    std::unique_lock<std::mutex> lock(record.mutex);
    EXPECT_TRUE(record.arrived.wait_for(lock, std::chrono::seconds(5), [&] {
      return record.updates >= 1;
    })) << "a publish did not reach the sink";
    EXPECT_EQ(record.last_uri, "ui://");
  }

  ASSERT_EQ(ihs_mcp_registry_set_notification_sink(nullptr, nullptr),
            IHS_MCP_OK);
  g_notify = nullptr;
}

// DR-8: an action reports what the tree looks like afterwards, so an agent
// gets act-and-verify in one round trip instead of acting blind and re-reading.
TEST_F(McpSemanticsProviderTest, ActionReportsThePostActionNode) {
  TreeBuilder before;
  before.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& slider =
      before.Add(12, "Temperature", IHS_SEMANTICS_ROLE_SLIDER);
  slider.actions = IHS_SEMANTICS_ACTION_INCREASE;
  slider.value = "21.0";
  ASSERT_EQ(before.Publish(), IHS_SEMANTICS_OK);

  // The framework handles the action and republishes, which is what makes the
  // new value visible at all.
  TreeBuilder after;
  host_.on_dispatch = [&after]() {
    after.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
    IhsSemanticsPublishNode& moved =
        after.Add(12, "Temperature", IHS_SEMANTICS_ROLE_SLIDER);
    moved.actions = IHS_SEMANTICS_ACTION_INCREASE;
    moved.value = "22.0";
    after.Publish();
  };

  int status = 0;
  const std::string body =
      CallTool("ui_increase", R"({"node_id":12})", &status);
  EXPECT_EQ(status, IHS_MCP_OK);
  EXPECT_TRUE(Contains(body, "\"mode\":\"semantics\"")) << body;
  EXPECT_TRUE(Contains(body, "\"node_after\"")) << body;
  // The point of the whole feature: the caller learns the new value without
  // a second call.
  EXPECT_TRUE(Contains(body, "22.0")) << body;
  EXPECT_FALSE(Contains(body, "21.0")) << body;
}

// A tap that navigates takes its own node out of the tree. That is a
// successful action, not a failure, so it is reported as null rather than an
// error a client would retry.
TEST_F(McpSemanticsProviderTest, NodeThatLeftTheTreeIsNullNotAnError) {
  TreeBuilder before;
  before.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& link =
      before.Add(7, "Next", IHS_SEMANTICS_ROLE_LINK);
  link.actions = IHS_SEMANTICS_ACTION_TAP;
  ASSERT_EQ(before.Publish(), IHS_SEMANTICS_OK);

  TreeBuilder route;
  host_.on_dispatch = [&route]() {
    route.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
    route.Add(99, "Second page", IHS_SEMANTICS_ROLE_PANE);
    route.Publish();
  };

  int status = 0;
  const std::string body = CallTool("ui_tap", R"({"node_id":7})", &status);
  EXPECT_EQ(status, IHS_MCP_OK) << body;
  EXPECT_TRUE(Contains(body, "\"node_after\":null")) << body;
}

// An action that changes nothing visible -- firing a network call, say --
// reports equal generations. That is an answer, and treating it as a failure
// would make a client retry something that already happened.
TEST_F(McpSemanticsProviderTest, NoTreeChangeIsAnAnswerNotAFailure) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& button =
      tree.Add(3, "Sync", IHS_SEMANTICS_ROLE_BUTTON);
  button.actions = IHS_SEMANTICS_ACTION_TAP;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  const std::string body = CallTool("ui_tap", R"({"node_id":3})", &status);
  EXPECT_EQ(status, IHS_MCP_OK) << body;
  EXPECT_EQ(host_.dispatch_calls, 1);
  // The node is still there and reported; only the generations say nothing
  // moved.
  EXPECT_TRUE(Contains(body, "\"node_after\"")) << body;
  EXPECT_FALSE(Contains(body, "\"node_after\":null")) << body;

  const size_t before_at = body.find("\"generation_before\":");
  const size_t after_at = body.find("\"generation_after\":");
  ASSERT_NE(before_at, std::string::npos) << body;
  ASSERT_NE(after_at, std::string::npos) << body;
  EXPECT_EQ(body.substr(before_at + 20, 3), body.substr(after_at + 19, 3))
      << body;
}

// A coordinate tap still names no target -- nothing knows what was under the
// point -- but it can say whether the tree moved, which is the only evidence
// of effect available to it.
TEST_F(McpSemanticsProviderTest, PointerTapReportsModeButNoNode) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  const std::string body =
      CallTool("ui_tap_at", R"({"x":10.0,"y":20.0})", &status);
  EXPECT_TRUE(Contains(body, "\"mode\":\"pointer\"")) << body;
  EXPECT_TRUE(Contains(body, "\"generation_after\"")) << body;
  EXPECT_FALSE(Contains(body, "\"node_after\"")) << body;
}

namespace {

// A node offering two application-defined verbs, which is the shape that
// makes name resolution worth testing at all.
void PublishWithCustomActions(TreeBuilder& tree) {
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& card = tree.Add(4, "Trip", IHS_SEMANTICS_ROLE_PANE);
  card.actions = IHS_SEMANTICS_ACTION_CUSTOM_ACTION;
  static const int32_t kIds[] = {71, 72};
  card.custom_action_ids = kIds;
  card.custom_action_count = 2;
  tree.AddCustomAction(71, "Share");
  tree.AddCustomAction(72, "Archive");
}

}  // namespace

// The app's own verbs are invoked by the label the tree reports, which is what
// an agent has: identifiers are absent at the deployment floor.
TEST_F(McpSemanticsProviderTest, CustomActionIsInvokedByLabel) {
  TreeBuilder tree;
  PublishWithCustomActions(tree);
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  const std::string body = CallTool(
      "ui_custom_action", R"({"node_id":4,"action":"Archive"})", &status);
  EXPECT_EQ(status, IHS_MCP_OK) << body;
  EXPECT_EQ(host_.dispatch_calls, 1);
  EXPECT_EQ(host_.last_action, IHS_SEMANTICS_ACTION_CUSTOM_ACTION);

  // The id travels as the argument: it is the only thing naming which verb to
  // run, and the framework resolves the handler by it.
  ASSERT_EQ(host_.last_data.size(), sizeof(int32_t));
  int32_t sent = 0;
  std::memcpy(&sent, host_.last_data.data(), sizeof(sent));
  EXPECT_EQ(sent, 72) << "dispatched the wrong verb";
}

TEST_F(McpSemanticsProviderTest, CustomActionMayBeNamedById) {
  TreeBuilder tree;
  PublishWithCustomActions(tree);
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  CallTool("ui_custom_action", R"({"node_id":4,"action_id":71})", &status);
  EXPECT_EQ(status, IHS_MCP_OK);
  int32_t sent = 0;
  ASSERT_EQ(host_.last_data.size(), sizeof(sent));
  std::memcpy(&sent, host_.last_data.data(), sizeof(sent));
  EXPECT_EQ(sent, 71);
}

// Resolution is against the node's own declarations. A verb another node
// declared is not this node's to run, and dispatching it would be answered by
// whichever handler the framework found rather than the one named.
TEST_F(McpSemanticsProviderTest, CustomActionDeclaredElsewhereIsRefused) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& first = tree.Add(4, "Trip", IHS_SEMANTICS_ROLE_PANE);
  first.actions = IHS_SEMANTICS_ACTION_CUSTOM_ACTION;
  static const int32_t kMine[] = {71};
  first.custom_action_ids = kMine;
  first.custom_action_count = 1;
  IhsSemanticsPublishNode& second =
      tree.Add(5, "Other", IHS_SEMANTICS_ROLE_PANE);
  second.actions = IHS_SEMANTICS_ACTION_CUSTOM_ACTION;
  static const int32_t kTheirs[] = {72};
  second.custom_action_ids = kTheirs;
  second.custom_action_count = 1;
  tree.AddCustomAction(71, "Share");
  tree.AddCustomAction(72, "Archive");
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  const std::string body = CallTool(
      "ui_custom_action", R"({"node_id":4,"action":"Archive"})", &status);
  EXPECT_EQ(status, IHS_MCP_ERR_NOT_FOUND) << body;
  EXPECT_EQ(host_.dispatch_calls, 0) << "dispatched another node's verb";
}

// A node that never declared any is refused before the label is even looked
// at, like every other action it does not offer.
TEST_F(McpSemanticsProviderTest, CustomActionOnANodeWithNoneIsRefused) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& plain =
      tree.Add(8, "Label", IHS_SEMANTICS_ROLE_LABEL);
  plain.actions = IHS_SEMANTICS_ACTION_TAP;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  CallTool("ui_custom_action", R"({"node_id":8,"action":"Share"})", &status);
  EXPECT_EQ(status, IHS_MCP_ERR_REFUSED);
  EXPECT_EQ(host_.dispatch_calls, 0);
}

TEST_F(McpSemanticsProviderTest, CustomActionWithoutANameIsRefused) {
  TreeBuilder tree;
  PublishWithCustomActions(tree);
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  CallTool("ui_custom_action", R"({"node_id":4})", &status);
  EXPECT_EQ(status, IHS_MCP_ERR_INVALID);
  EXPECT_EQ(host_.dispatch_calls, 0);
}

// Finding what can be acted on is the first thing an agent does, and the
// action names are the ones the tree already reported on each node.
TEST_F(McpSemanticsProviderTest, QuerySelectsByActionOffered) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& button =
      tree.Add(1, "Fan", IHS_SEMANTICS_ROLE_BUTTON);
  button.actions = IHS_SEMANTICS_ACTION_TAP;
  IhsSemanticsPublishNode& slider =
      tree.Add(2, "Temp", IHS_SEMANTICS_ROLE_SLIDER);
  slider.actions = IHS_SEMANTICS_ACTION_INCREASE;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  const std::string body =
      CallTool("ui_query", R"({"action":"increase"})", &status);
  EXPECT_EQ(status, IHS_MCP_OK);
  EXPECT_TRUE(Contains(body, "Temp")) << body;
  EXPECT_FALSE(Contains(body, "Fan")) << body;
}

// A zero mask tests true against every node, so an unknown action name would
// select the whole tree -- the opposite of what was asked for.
TEST_F(McpSemanticsProviderTest, QueryWithAnUnknownActionIsAnError) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  tree.Add(1, "Fan", IHS_SEMANTICS_ROLE_BUTTON);
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  const std::string body =
      CallTool("ui_query", R"({"action":"levitate"})", nullptr);
  EXPECT_TRUE(Contains(body, "no such action")) << body;
  EXPECT_FALSE(Contains(body, "\"label\":\"Fan\"")) << body;
}

// The exit criterion drives flows by custom-action name, so finding the node
// that offers one has to be a query rather than a walk of the whole tree.
TEST_F(McpSemanticsProviderTest, QuerySelectsByCustomActionLabel) {
  TreeBuilder tree;
  PublishWithCustomActions(tree);
  IhsSemanticsPublishNode& other =
      tree.Add(9, "Plain", IHS_SEMANTICS_ROLE_PANE);
  other.actions = IHS_SEMANTICS_ACTION_TAP;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  const std::string body =
      CallTool("ui_query", R"({"custom_action":"Archive"})", nullptr);
  EXPECT_TRUE(Contains(body, "Trip")) << body;
  EXPECT_FALSE(Contains(body, "Plain")) << body;
}

// Selected by tristate name, not a bool: the hub keeps "disabled" and
// "enablement does not apply" apart, and a bool would rejoin them here.
TEST_F(McpSemanticsProviderTest, QuerySelectsByEnabledTristate) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& live =
      tree.Add(1, "Live", IHS_SEMANTICS_ROLE_BUTTON);
  live.enabled = IHS_SEMANTICS_TRISTATE_TRUE;
  IhsSemanticsPublishNode& dead =
      tree.Add(2, "Dead", IHS_SEMANTICS_ROLE_BUTTON);
  dead.enabled = IHS_SEMANTICS_TRISTATE_FALSE;
  IhsSemanticsPublishNode& moot = tree.Add(3, "Moot", IHS_SEMANTICS_ROLE_LABEL);
  moot.enabled = IHS_SEMANTICS_TRISTATE_NONE;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  const std::string disabled =
      CallTool("ui_query", R"({"enabled":"false"})", nullptr);
  EXPECT_TRUE(Contains(disabled, "Dead")) << disabled;
  EXPECT_FALSE(Contains(disabled, "Live")) << disabled;
  EXPECT_FALSE(Contains(disabled, "Moot")) << disabled;

  // Spelled exactly as the tree reports it -- the selector accepting a value
  // the serializer never emits would match nothing and look like "no such
  // nodes".
  const std::string inapplicable =
      CallTool("ui_query", R"({"enabled":"not_applicable"})", nullptr);
  EXPECT_TRUE(Contains(inapplicable, "Moot")) << inapplicable;
  EXPECT_FALSE(Contains(inapplicable, "Dead")) << inapplicable;
}

// Selectors narrow together; that is what makes having several worth anything.
TEST_F(McpSemanticsProviderTest, QuerySelectorsCombine) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  IhsSemanticsPublishNode& right =
      tree.Add(1, "Fan speed", IHS_SEMANTICS_ROLE_SLIDER);
  right.actions = IHS_SEMANTICS_ACTION_INCREASE;
  IhsSemanticsPublishNode& wrong_role =
      tree.Add(2, "Fan speed", IHS_SEMANTICS_ROLE_BUTTON);
  wrong_role.actions = IHS_SEMANTICS_ACTION_INCREASE;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  const std::string body = CallTool(
      "ui_query", R"({"label":"Fan","role":"slider","action":"increase"})",
      nullptr);
  EXPECT_TRUE(Contains(body, "\"id\":1")) << body;
  EXPECT_FALSE(Contains(body, "\"id\":2")) << body;
}

// A cap that looked like a complete answer would have an agent conclude the
// rest does not exist, so the total is reported alongside the truncation.
TEST_F(McpSemanticsProviderTest, QueryLimitReportsWhatItLeftOut) {
  TreeBuilder tree;
  tree.Add(0, "root", IHS_SEMANTICS_ROLE_WINDOW);
  for (int32_t i = 1; i <= 5; i++) {
    tree.Add(i, "Seat", IHS_SEMANTICS_ROLE_BUTTON);
  }
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  const std::string body =
      CallTool("ui_query", R"({"label":"Seat","limit":2})", nullptr);
  EXPECT_TRUE(Contains(body, "\"match_count\":5")) << body;
  EXPECT_TRUE(Contains(body, "\"truncated\":true")) << body;

  const std::string whole =
      CallTool("ui_query", R"({"label":"Seat"})", nullptr);
  EXPECT_TRUE(Contains(whole, "\"match_count\":5")) << whole;
  EXPECT_TRUE(Contains(whole, "\"truncated\":false")) << whole;
}

// ---------------------------------------------------------------------------
// Argument decoding
//
// The provider is handed (pointer, length) and must honour both. It used to
// scan the raw text for a quoted key, which read to a terminator the ABI does
// not promise and could not tell a key from a key-like substring inside a
// value a client chose. Fuzzing found the first; the second falls out of the
// same fix.
// ---------------------------------------------------------------------------

// A key nested inside another object is not a top-level argument. A scanner
// that searches the raw text cannot tell the two apart, so a request naming no
// node_id of its own acted on whatever id appeared anywhere in the JSON -- the
// tool then reports success against a node the arguments never asked for.
TEST_F(McpSemanticsProviderTest, ANestedKeyIsNotAnArgument) {
  TreeBuilder tree;
  tree.Add(1, "Root", IHS_SEMANTICS_ROLE_PANE);
  auto& target = tree.Add(2, "Target", IHS_SEMANTICS_ROLE_BUTTON);
  target.actions = IHS_SEMANTICS_ACTION_TAP;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  const std::string body = CallTool(
      "ui_tap", R"({"identifier":"nothing","meta":{"node_id":2}})", &status);

  EXPECT_EQ(host_.dispatch_calls, 0)
      << "acted on a node the arguments never named: " << body;
  EXPECT_EQ(status, IHS_MCP_ERR_NOT_FOUND) << body;
}

// The scanner took the run between the next two quote characters, so a
// selector containing an escaped quote silently became a prefix of itself --
// and a prefix matches more nodes than the caller asked for.
TEST_F(McpSemanticsProviderTest, AnEscapedQuoteInASelectorSurvives) {
  TreeBuilder tree;
  tree.Add(1, "Root", IHS_SEMANTICS_ROLE_PANE);
  tree.Add(2, R"(He said "go")", IHS_SEMANTICS_ROLE_LABEL);
  tree.Add(3, "He said nothing", IHS_SEMANTICS_ROLE_LABEL);
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  const std::string body =
      CallTool("ui_query", R"({"label":"said \"go\""})", nullptr);

  EXPECT_TRUE(Contains(body, R"(He said \"go\")")) << body;
  EXPECT_FALSE(Contains(body, "He said nothing"))
      << "the selector was truncated at the escape: " << body;
}

// Not a regression -- the previous extractor range-checked too. Kept because
// the rewrite could quietly have lost it, and truncating an out-of-range id
// lands on some other node: the caller would be told an action succeeded
// against something it never named.
TEST_F(McpSemanticsProviderTest, AnOutOfRangeNodeIdIsStillRefused) {
  TreeBuilder tree;
  tree.Add(1, "Root", IHS_SEMANTICS_ROLE_PANE);
  auto& target = tree.Add(2, "Target", IHS_SEMANTICS_ROLE_BUTTON);
  target.actions = IHS_SEMANTICS_ACTION_TAP;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  int status = 0;
  const std::string body =
      CallTool("ui_tap", R"({"node_id":4294967298})", &status);
  EXPECT_EQ(host_.dispatch_calls, 0) << body;
  EXPECT_EQ(status, IHS_MCP_ERR_NOT_FOUND) << body;
}

// The ABI hands over a pointer and a length. Nothing in it promises a
// terminator, so a caller passing a slice of a larger buffer is within
// contract, and the provider must not read past the length it was given.
//
// The arguments are placed so they end exactly at a page boundary with the
// next page unmapped. A plain heap buffer would not do: reading past it lands
// on adjacent heap and parses on regardless, so the test would pass under a
// build without a sanitizer and prove nothing. Here a single byte too far is
// a fault in every build, which is the point -- this is the one test standing
// between the ABI's promise and a caller who takes it literally.
TEST_F(McpSemanticsProviderTest, ArgumentsAreReadWithinTheirLength) {
  TreeBuilder tree;
  tree.Add(1, "Root", IHS_SEMANTICS_ROLE_PANE);
  auto& target = tree.Add(2, "Target", IHS_SEMANTICS_ROLE_BUTTON);
  target.actions = IHS_SEMANTICS_ACTION_TAP;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  const std::string json = R"({"node_id":2})";
  const auto page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
  ASSERT_GT(page, json.size());

  char* region =
      static_cast<char*>(::mmap(nullptr, page * 2, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  ASSERT_NE(region, MAP_FAILED);
  ASSERT_EQ(::mprotect(region + page, page, PROT_NONE), 0);

  char* arguments = region + page - json.size();
  std::memcpy(arguments, json.data(), json.size());

  IhsMcpPayload payload{};
  const int status =
      ihs_mcp_registry_call_tool("ui_tap", arguments, json.size(), &payload);
  ihs_mcp_registry_release_payload(&payload);

  EXPECT_EQ(status, IHS_MCP_OK);
  EXPECT_EQ(host_.dispatch_calls, 1);
  EXPECT_EQ(host_.last_node_id, 2);

  ::munmap(region, page * 2);
}

// ---------------------------------------------------------------------------
// Tool policy
//
// An image can narrow what the surface offers. The compiled default is
// already correct, so this only ever takes away -- a configuration file that
// has to be present for the shell to be safe is one whose absence is a
// vulnerability.
//
// The list is a gate rather than a suggestion because the registry resolves a
// call against list_tools before routing it. That is what makes narrowing the
// advertised set sufficient for ui_tap_at, which sends a pointer event rather
// than a semantics action and so has nothing for the hub mask to enforce.
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> OfferedTools() {
  std::vector<std::string> names;
  ihs_mcp_registry_for_each_tool(
      [](const IhsMcpRegistryTool* tool, void* user_data) {
        static_cast<std::vector<std::string>*>(user_data)->emplace_back(
            tool->name);
      },
      &names);
  return names;
}

bool Offers(const std::vector<std::string>& names, const char* tool) {
  return std::find(names.begin(), names.end(), tool) != names.end();
}

// Restarts the provider under a policy. The fixture already started it, and
// start is idempotent by design -- the policy of the first successful start is
// the one in force -- so a restart is how a different one is applied.
int RestartWith(const std::vector<const char*>& allowed, bool narrowed) {
  ihs_mcp_semantics_provider_stop();
  IhsMcpSemanticsConfig config{};
  config.struct_size = sizeof(config);
  config.narrow_tools = narrowed;
  config.allowed_tools = allowed.empty() ? nullptr : allowed.data();
  config.allowed_tool_count = allowed.size();
  return ihs_mcp_semantics_provider_start_with(&config);
}

}  // namespace

TEST_F(McpSemanticsProviderTest, NoPolicyOffersEverything) {
  const std::vector<std::string> names = OfferedTools();
  EXPECT_TRUE(Offers(names, "ui_tap"));
  EXPECT_TRUE(Offers(names, "ui_set_text"));
  EXPECT_TRUE(Offers(names, "ui_tap_at"));
  EXPECT_TRUE(Offers(names, "ui_snapshot"));
}

// The read-only surface. Reading survives because that is what the tree is
// for: an agent that cannot read it has nothing to work with, and would see a
// tool list it could not explain.
TEST_F(McpSemanticsProviderTest, AnEmptyPolicyLeavesOnlyReading) {
  ASSERT_EQ(RestartWith({}, /*narrowed=*/true), IHS_MCP_OK);
  const std::vector<std::string> names = OfferedTools();
  EXPECT_TRUE(Offers(names, "ui_snapshot"));
  EXPECT_TRUE(Offers(names, "ui_query"));
  EXPECT_FALSE(Offers(names, "ui_tap"));
  EXPECT_FALSE(Offers(names, "ui_set_text"));
  EXPECT_FALSE(Offers(names, "ui_tap_at"))
      << "the coordinate tap survived a read-only policy; the hub mask cannot "
         "stop it, so the tool list is the only thing that can";
}

TEST_F(McpSemanticsProviderTest, APolicyOffersWhatItNamesAndNothingElse) {
  ASSERT_EQ(RestartWith({"tap", "scroll_to"}, /*narrowed=*/true), IHS_MCP_OK);
  const std::vector<std::string> names = OfferedTools();
  EXPECT_TRUE(Offers(names, "ui_tap"));
  EXPECT_TRUE(Offers(names, "ui_scroll_to"));
  EXPECT_FALSE(Offers(names, "ui_set_text"));
  EXPECT_FALSE(Offers(names, "ui_long_press"));
}

// Not advertised means not callable, because the registry resolves the name
// against the listing before it routes. Worth asserting rather than assuming:
// if that ever stopped being true, every narrowed policy would be decoration.
TEST_F(McpSemanticsProviderTest, ANarrowedOutToolCannotBeCalled) {
  TreeBuilder tree;
  tree.Add(1, "Root", IHS_SEMANTICS_ROLE_PANE);
  auto& target = tree.Add(2, "Target", IHS_SEMANTICS_ROLE_BUTTON);
  target.actions = IHS_SEMANTICS_ACTION_TAP;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);
  ASSERT_EQ(RestartWith({}, /*narrowed=*/true), IHS_MCP_OK);

  int status = 0;
  const std::string body = CallTool("ui_tap", R"({"node_id":2})", &status);
  EXPECT_EQ(status, IHS_MCP_ERR_NOT_FOUND) << body;
  EXPECT_EQ(host_.dispatch_calls, 0)
      << "a tool that is not offered still reached the framework: " << body;
}

// A name matching no tool refuses the start outright. Skipping it would grant
// less than the image asked for and say nothing; the failure mode this avoids
// is a policy file whose typo silently produced a different surface.
TEST_F(McpSemanticsProviderTest, AnUnknownToolNameRefusesTheStart) {
  ihs_mcp_semantics_provider_stop();
  const std::vector<const char*> allowed = {"tap", "tapp"};
  IhsMcpSemanticsConfig config{};
  config.struct_size = sizeof(config);
  config.narrow_tools = true;
  config.allowed_tools = allowed.data();
  config.allowed_tool_count = allowed.size();

  EXPECT_EQ(ihs_mcp_semantics_provider_start_with(&config),
            IHS_MCP_ERR_INVALID);
  EXPECT_FALSE(ihs_mcp_semantics_provider_running())
      << "a policy that could not be applied left a surface running under "
         "some other one";
  EXPECT_EQ(ihs_mcp_provider_count(), 0u);

  // The fixture's teardown expects to be able to stop cleanly either way.
  ASSERT_EQ(ihs_mcp_semantics_provider_start(), IHS_MCP_OK);
}

// A config from a caller built against an older header is refused rather than
// read past. The struct carries its own size for exactly this.
TEST_F(McpSemanticsProviderTest, AShortConfigIsRefused) {
  ihs_mcp_semantics_provider_stop();
  IhsMcpSemanticsConfig config{};
  config.struct_size = sizeof(config) - 1;
  EXPECT_EQ(ihs_mcp_semantics_provider_start_with(&config),
            IHS_MCP_ERR_INVALID);
  ASSERT_EQ(ihs_mcp_semantics_provider_start(), IHS_MCP_OK);
}

// The mask handed to the hub is derived from the tool table, so a tool that
// dispatches more than its primary action has to say so. Two do: custom_action
// routes itself, and set_text focuses the field first because the framework
// ignores text sent to a field with no input connection.
//
// Deriving the mask from the primary action alone leaves those advertised and
// then refused at the funnel, which reads to a client as the surface being
// broken rather than as policy. This exercises both under a narrowed policy,
// where the mask is at its tightest and the omission would show.
TEST_F(McpSemanticsProviderTest, ANarrowedPolicyStillAllowsWhatItsToolsNeed) {
  TreeBuilder tree;
  tree.Add(1, "Root", IHS_SEMANTICS_ROLE_PANE);
  auto& field = tree.Add(2, "Destination", IHS_SEMANTICS_ROLE_TEXT_INPUT);
  field.actions = IHS_SEMANTICS_ACTION_SET_TEXT | IHS_SEMANTICS_ACTION_FOCUS;
  auto& button = tree.Add(3, "Climate", IHS_SEMANTICS_ROLE_BUTTON);
  button.actions = IHS_SEMANTICS_ACTION_CUSTOM_ACTION;
  static const int32_t kIds[] = {11};
  button.custom_action_ids = kIds;
  button.custom_action_count = 1;
  tree.AddCustomAction(11, "Set to maximum");
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  ASSERT_EQ(RestartWith({"set_text", "custom_action"}, /*narrowed=*/true),
            IHS_MCP_OK);

  int status = 0;
  CallTool("ui_set_text", R"({"node_id":2,"text":"Portland"})", &status);
  EXPECT_EQ(status, IHS_MCP_OK)
      << "set_text was offered but the mask refused what it needs";

  status = 0;
  const std::string body =
      CallTool("ui_custom_action", R"({"node_id":3,"action":"Set to maximum"})",
               &status);
  // The specific expectation, not the absence of one guessed error code: a
  // mask missing this action refuses at the funnel, which surfaces as
  // "refused" rather than "capability denied", and asserting the latter would
  // pass straight over it.
  EXPECT_EQ(status, IHS_MCP_OK)
      << "custom_action was offered but the mask does not carry its action: "
      << body;
}
