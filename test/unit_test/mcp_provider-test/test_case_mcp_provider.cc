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

#include <cstring>
#include <string>
#include <vector>

#include <sys/eventfd.h>
#include <unistd.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include "gtest/gtest.h"

#include "ihs/ihs_mcp_host.h"
#include "ihs/ihs_mcp_provider.h"

namespace {

// A provider with a fixed tool and resource set. Records what the registry
// asked of it, so a test can assert not just the answer but that the routing
// reached the right place with the right name.
struct MockProvider {
  std::vector<IhsMcpToolDesc> tools;
  std::vector<IhsMcpResourceDesc> resources;

  std::string last_tool_called;
  std::string last_arguments;
  std::string last_uri_read;
  std::string last_uri_subscribed;
  bool last_subscribed_state = false;
  int call_tool_result = IHS_MCP_OK;
  int subscribe_calls = 0;

  static int ListTools(void* user_data,
                       const IhsMcpToolDesc** out_tools,
                       size_t* out_count) {
    auto* self = static_cast<MockProvider*>(user_data);
    *out_tools = self->tools.empty() ? nullptr : self->tools.data();
    *out_count = self->tools.size();
    return IHS_MCP_OK;
  }

  static int CallTool(void* user_data,
                      const char* name,
                      const char* arguments_json,
                      size_t arguments_length,
                      IhsMcpPayload* out_result) {
    auto* self = static_cast<MockProvider*>(user_data);
    self->last_tool_called = name;
    self->last_arguments.assign(arguments_json, arguments_length);
    if (self->call_tool_result != IHS_MCP_OK) {
      return self->call_tool_result;
    }
    static const char kReply[] = "{\"ok\":true}";
    out_result->struct_size = sizeof(IhsMcpPayload);
    out_result->data = kReply;
    out_result->length = sizeof(kReply) - 1;
    out_result->release = nullptr;
    out_result->release_ctx = nullptr;
    return IHS_MCP_OK;
  }

  static int ListResources(void* user_data,
                           const IhsMcpResourceDesc** out_resources,
                           size_t* out_count) {
    auto* self = static_cast<MockProvider*>(user_data);
    *out_resources = self->resources.empty() ? nullptr : self->resources.data();
    *out_count = self->resources.size();
    return IHS_MCP_OK;
  }

  static int ReadResource(void* user_data,
                          const char* uri,
                          IhsMcpPayload* out_content) {
    auto* self = static_cast<MockProvider*>(user_data);
    self->last_uri_read = uri;
    static const char kBody[] = "{}";
    out_content->struct_size = sizeof(IhsMcpPayload);
    out_content->data = kBody;
    out_content->length = sizeof(kBody) - 1;
    out_content->release = nullptr;
    out_content->release_ctx = nullptr;
    return IHS_MCP_OK;
  }

  static int Subscribe(void* user_data, const char* uri, bool subscribed) {
    auto* self = static_cast<MockProvider*>(user_data);
    self->last_uri_subscribed = uri;
    self->last_subscribed_state = subscribed;
    self->subscribe_calls++;
    return IHS_MCP_OK;
  }

  IhsMcpProviderDesc Desc(const char* name,
                          const char* tool_prefix,
                          const char* scheme,
                          uint64_t mask) {
    IhsMcpProviderDesc desc{};
    desc.struct_size = sizeof(IhsMcpProviderDesc);
    desc.name = name;
    desc.tool_prefix = tool_prefix;
    desc.resource_scheme = scheme;
    desc.capability_mask = mask;
    desc.callbacks.list_tools = ListTools;
    desc.callbacks.call_tool = CallTool;
    desc.callbacks.list_resources = ListResources;
    desc.callbacks.read_resource = ReadResource;
    desc.callbacks.subscribe = Subscribe;
    desc.user_data = this;
    desc.notify_fd = -1;
    return desc;
  }
};

IhsMcpToolDesc Tool(const char* name, uint64_t capability) {
  IhsMcpToolDesc tool{};
  tool.name = name;
  tool.description = "";
  tool.input_schema_json = R"({"type":"object"})";
  tool.capability = capability;
  return tool;
}

std::vector<std::string> ListedToolNames() {
  std::vector<std::string> names;
  ihs_mcp_host_for_each_tool(
      [](const IhsMcpHostTool* tool, void* user_data) {
        static_cast<std::vector<std::string>*>(user_data)->emplace_back(
            tool->name);
      },
      &names);
  return names;
}

// The registry is process-global, so every test must leave it empty.
class McpProviderTest : public ::testing::Test {
 protected:
  void TearDown() override {
    for (IhsMcpProvider* provider : registered_) {
      ihs_mcp_provider_unregister(provider);
    }
    registered_.clear();
    ASSERT_EQ(ihs_mcp_provider_count(), 0u);
  }

  IhsMcpProvider* Register(const IhsMcpProviderDesc& desc) {
    IhsMcpProvider* provider = nullptr;
    EXPECT_EQ(ihs_mcp_provider_register(&desc, &provider), IHS_MCP_OK);
    if (provider != nullptr) {
      registered_.push_back(provider);
    }
    return provider;
  }

  std::vector<IhsMcpProvider*> registered_;
};

}  // namespace

TEST_F(McpProviderTest, RegisterRejectsMalformedDesc) {
  IhsMcpProvider* provider = nullptr;
  EXPECT_EQ(ihs_mcp_provider_register(nullptr, &provider), IHS_MCP_ERR_INVALID);

  MockProvider mock;
  IhsMcpProviderDesc desc = mock.Desc("m", "ui_", "ui", IHS_MCP_CAP_ALL);
  EXPECT_EQ(ihs_mcp_provider_register(&desc, nullptr), IHS_MCP_ERR_INVALID);

  desc.name = nullptr;
  EXPECT_EQ(ihs_mcp_provider_register(&desc, &provider), IHS_MCP_ERR_INVALID);
  EXPECT_EQ(ihs_mcp_provider_count(), 0u);
}

// An empty prefix claims the whole namespace and would collide with every
// provider registered after it, so it is refused rather than becoming a
// first-come land grab.
TEST_F(McpProviderTest, EmptyPrefixIsRejected) {
  MockProvider mock;
  IhsMcpProvider* provider = nullptr;

  IhsMcpProviderDesc no_tool = mock.Desc("m", "", "ui", IHS_MCP_CAP_ALL);
  EXPECT_EQ(ihs_mcp_provider_register(&no_tool, &provider),
            IHS_MCP_ERR_INVALID);

  IhsMcpProviderDesc no_scheme = mock.Desc("m", "ui_", "", IHS_MCP_CAP_ALL);
  EXPECT_EQ(ihs_mcp_provider_register(&no_scheme, &provider),
            IHS_MCP_ERR_INVALID);
  EXPECT_EQ(ihs_mcp_provider_count(), 0u);
}

TEST_F(McpProviderTest, DuplicatePrefixIsRejected) {
  MockProvider first;
  MockProvider second;
  Register(first.Desc("first", "ui_", "ui", IHS_MCP_CAP_ALL));

  IhsMcpProviderDesc clash =
      second.Desc("second", "ui_", "scene", IHS_MCP_CAP_ALL);
  IhsMcpProvider* provider = nullptr;
  EXPECT_EQ(ihs_mcp_provider_register(&clash, &provider),
            IHS_MCP_ERR_PREFIX_TAKEN);
  EXPECT_EQ(ihs_mcp_provider_count(), 1u);
}

// Overlap, not just equality: "ui_" and "ui_debug_" would both match a tool
// named "ui_debug_dump" and which provider received it would come down to
// registration order.
TEST_F(McpProviderTest, OverlappingPrefixIsRejectedInBothDirections) {
  MockProvider first;
  MockProvider second;
  Register(first.Desc("first", "ui_", "ui", IHS_MCP_CAP_ALL));

  IhsMcpProviderDesc longer =
      second.Desc("second", "ui_debug_", "scene", IHS_MCP_CAP_ALL);
  IhsMcpProvider* provider = nullptr;
  EXPECT_EQ(ihs_mcp_provider_register(&longer, &provider),
            IHS_MCP_ERR_PREFIX_TAKEN);

  // And the other way round: register the longer one first.
  TearDown();
  MockProvider third;
  MockProvider fourth;
  Register(third.Desc("third", "ui_debug_", "dbg", IHS_MCP_CAP_ALL));
  IhsMcpProviderDesc shorter =
      fourth.Desc("fourth", "ui_", "ui", IHS_MCP_CAP_ALL);
  EXPECT_EQ(ihs_mcp_provider_register(&shorter, &provider),
            IHS_MCP_ERR_PREFIX_TAKEN);
}

// A rejected registration must leave nothing behind, or a caller ignoring the
// status ends up with resources served and tools missing.
TEST_F(McpProviderTest, RejectedRegistrationIsNotPartiallyApplied) {
  MockProvider first;
  MockProvider second;
  first.tools.push_back(Tool("tap", IHS_MCP_CAP_INTERACT));
  Register(first.Desc("first", "ui_", "ui", IHS_MCP_CAP_ALL));

  second.tools.push_back(Tool("render", IHS_MCP_CAP_INTERACT));
  IhsMcpProviderDesc clash =
      second.Desc("second", "ui_", "scene", IHS_MCP_CAP_ALL);
  IhsMcpProvider* provider = nullptr;
  ASSERT_EQ(ihs_mcp_provider_register(&clash, &provider),
            IHS_MCP_ERR_PREFIX_TAKEN);

  const std::vector<std::string> names = ListedToolNames();
  ASSERT_EQ(names.size(), 1u);
  EXPECT_EQ(names[0], "ui_tap");
}

// Providers never repeat their own namespace, and cannot advertise outside it.
TEST_F(McpProviderTest, ToolNamesAreAdvertisedWithTheProviderPrefix) {
  MockProvider mock;
  mock.tools.push_back(Tool("tap", IHS_MCP_CAP_INTERACT));
  mock.tools.push_back(Tool("snapshot", IHS_MCP_CAP_INSPECT));
  Register(mock.Desc("semantics", "ui_", "ui", IHS_MCP_CAP_ALL));

  const std::vector<std::string> names = ListedToolNames();
  ASSERT_EQ(names.size(), 2u);
  EXPECT_EQ(names[0], "ui_tap");
  EXPECT_EQ(names[1], "ui_snapshot");
}

// Masking happens at advertisement, so a masked tool is not discoverable at
// all rather than merely refused when called.
TEST_F(McpProviderTest, MaskedOutToolsAreNeverAdvertised) {
  MockProvider mock;
  mock.tools.push_back(Tool("snapshot", IHS_MCP_CAP_INSPECT));
  mock.tools.push_back(Tool("tap", IHS_MCP_CAP_INTERACT));
  mock.tools.push_back(Tool("poke_ecs", IHS_MCP_CAP_PRIVILEGED));
  Register(mock.Desc("semantics", "ui_", "ui", IHS_MCP_CAP_READ_ONLY));

  const std::vector<std::string> names = ListedToolNames();
  ASSERT_EQ(names.size(), 1u);
  EXPECT_EQ(names[0], "ui_snapshot");
}

// A tool that declares no capability is underspecified, not unrestricted.
// Treating an omission as permission would make forgetting the field the most
// permissive thing a provider could do.
TEST_F(McpProviderTest, ToolWithNoDeclaredCapabilityIsRefused) {
  MockProvider mock;
  mock.tools.push_back(Tool("mystery", 0));
  Register(mock.Desc("semantics", "ui_", "ui", IHS_MCP_CAP_ALL));

  EXPECT_TRUE(ListedToolNames().empty());

  IhsMcpPayload payload{};
  EXPECT_EQ(ihs_mcp_host_call_tool("ui_mystery", nullptr, 0, &payload),
            IHS_MCP_ERR_CAPABILITY_DENIED);
}

TEST_F(McpProviderTest, CallRoutesByPrefixAndStripsIt) {
  MockProvider ui;
  MockProvider scene;
  ui.tools.push_back(Tool("tap", IHS_MCP_CAP_INTERACT));
  scene.tools.push_back(Tool("tap", IHS_MCP_CAP_INTERACT));
  Register(ui.Desc("semantics", "ui_", "ui", IHS_MCP_CAP_ALL));
  Register(scene.Desc("scene", "scene_", "scene", IHS_MCP_CAP_ALL));

  IhsMcpPayload payload{};
  ASSERT_EQ(ihs_mcp_host_call_tool("scene_tap", "{\"a\":1}", 7, &payload),
            IHS_MCP_OK);
  // Reached the right provider, and saw its own unprefixed vocabulary.
  EXPECT_EQ(scene.last_tool_called, "tap");
  EXPECT_EQ(scene.last_arguments, "{\"a\":1}");
  EXPECT_TRUE(ui.last_tool_called.empty());
  ihs_mcp_host_release_payload(&payload);
}

// Providers are promised a non-null argument object, so the registry supplies
// one rather than every provider repeating the same null check.
TEST_F(McpProviderTest, AbsentArgumentsBecomeAnEmptyObject) {
  MockProvider mock;
  mock.tools.push_back(Tool("snapshot", IHS_MCP_CAP_INSPECT));
  Register(mock.Desc("semantics", "ui_", "ui", IHS_MCP_CAP_ALL));

  IhsMcpPayload payload{};
  ASSERT_EQ(ihs_mcp_host_call_tool("ui_snapshot", nullptr, 0, &payload),
            IHS_MCP_OK);
  EXPECT_EQ(mock.last_arguments, "{}");
  ihs_mcp_host_release_payload(&payload);
}

TEST_F(McpProviderTest, UnknownToolAndUnknownPrefixAreNotFound) {
  MockProvider mock;
  mock.tools.push_back(Tool("tap", IHS_MCP_CAP_INTERACT));
  Register(mock.Desc("semantics", "ui_", "ui", IHS_MCP_CAP_ALL));

  IhsMcpPayload payload{};
  EXPECT_EQ(ihs_mcp_host_call_tool("ui_nonexistent", nullptr, 0, &payload),
            IHS_MCP_ERR_NOT_FOUND);
  EXPECT_EQ(ihs_mcp_host_call_tool("scene_tap", nullptr, 0, &payload),
            IHS_MCP_ERR_NOT_FOUND);
}

// The mask must not be bypassable by a provider declining to enumerate: if
// nothing can be checked, nothing is permitted.
TEST_F(McpProviderTest, ProviderWithoutListingCannotHaveToolsCalled) {
  MockProvider mock;
  IhsMcpProviderDesc desc =
      mock.Desc("semantics", "ui_", "ui", IHS_MCP_CAP_ALL);
  desc.callbacks.list_tools = nullptr;
  Register(desc);

  IhsMcpPayload payload{};
  EXPECT_EQ(ihs_mcp_host_call_tool("ui_tap", nullptr, 0, &payload),
            IHS_MCP_ERR_CAPABILITY_DENIED);
  EXPECT_TRUE(mock.last_tool_called.empty());
}

// A provider's own refusal is passed back verbatim rather than flattened, so a
// client can tell "the provider declined" from "the registry could not route".
TEST_F(McpProviderTest, ProviderRefusalIsReportedVerbatim) {
  MockProvider mock;
  mock.tools.push_back(Tool("tap", IHS_MCP_CAP_INTERACT));
  mock.call_tool_result = IHS_MCP_ERR_REFUSED;
  Register(mock.Desc("semantics", "ui_", "ui", IHS_MCP_CAP_ALL));

  IhsMcpPayload payload{};
  EXPECT_EQ(ihs_mcp_host_call_tool("ui_tap", nullptr, 0, &payload),
            IHS_MCP_ERR_REFUSED);
}

TEST_F(McpProviderTest, ResourcesAreListedAndRoutedByScheme) {
  MockProvider ui;
  MockProvider scene;
  IhsMcpResourceDesc tree{};
  tree.uri = "ui://semantics/tree";
  tree.name = "tree";
  tree.description = "";
  tree.mime_type = "application/json";
  ui.resources.push_back(tree);

  IhsMcpResourceDesc graph{};
  graph.uri = "scene://graph";
  graph.name = "graph";
  graph.description = "";
  graph.mime_type = "application/json";
  scene.resources.push_back(graph);

  Register(ui.Desc("semantics", "ui_", "ui", IHS_MCP_CAP_ALL));
  Register(scene.Desc("scene", "scene_", "scene", IHS_MCP_CAP_ALL));

  std::vector<std::string> uris;
  ASSERT_EQ(
      ihs_mcp_host_for_each_resource(
          [](const IhsMcpHostResource* resource, void* user_data) {
            static_cast<std::vector<std::string>*>(user_data)->emplace_back(
                resource->uri);
          },
          &uris),
      IHS_MCP_OK);
  ASSERT_EQ(uris.size(), 2u);

  IhsMcpPayload payload{};
  ASSERT_EQ(ihs_mcp_host_read_resource("scene://graph", &payload), IHS_MCP_OK);
  EXPECT_EQ(scene.last_uri_read, "scene://graph");
  EXPECT_TRUE(ui.last_uri_read.empty());
  ihs_mcp_host_release_payload(&payload);

  EXPECT_EQ(ihs_mcp_host_read_resource("nope://x", &payload),
            IHS_MCP_ERR_NOT_FOUND);
}

TEST_F(McpProviderTest, SubscribeRoutesByScheme) {
  MockProvider mock;
  Register(mock.Desc("semantics", "ui_", "ui", IHS_MCP_CAP_ALL));

  EXPECT_EQ(ihs_mcp_host_subscribe("ui://semantics/tree", true), IHS_MCP_OK);
  EXPECT_EQ(mock.last_uri_subscribed, "ui://semantics/tree");
  EXPECT_TRUE(mock.last_subscribed_state);

  EXPECT_EQ(ihs_mcp_host_subscribe("ui://semantics/tree", false), IHS_MCP_OK);
  EXPECT_FALSE(mock.last_subscribed_state);
  EXPECT_EQ(ihs_mcp_host_subscribe("nope://x", true), IHS_MCP_ERR_NOT_FOUND);
}

// A provider that is always current may leave subscribe NULL; the registry
// tracks interest itself, so that is a success rather than a gap.
TEST_F(McpProviderTest, ProviderWithoutSubscribeSucceedsAnyway) {
  MockProvider mock;
  IhsMcpProviderDesc desc =
      mock.Desc("semantics", "ui_", "ui", IHS_MCP_CAP_ALL);
  desc.callbacks.subscribe = nullptr;
  Register(desc);

  EXPECT_EQ(ihs_mcp_host_subscribe("ui://semantics/tree", true), IHS_MCP_OK);
  EXPECT_EQ(mock.subscribe_calls, 0);
}

// A resources-only provider needs no tool stubs: an absent capability is an
// empty list, not an error that would break listing for everyone else.
TEST_F(McpProviderTest, ResourcesOnlyProviderContributesNoTools) {
  MockProvider resources_only;
  MockProvider tools;
  IhsMcpProviderDesc desc =
      resources_only.Desc("res", "res_", "res", IHS_MCP_CAP_ALL);
  desc.callbacks.list_tools = nullptr;
  Register(desc);

  tools.tools.push_back(Tool("tap", IHS_MCP_CAP_INTERACT));
  Register(tools.Desc("semantics", "ui_", "ui", IHS_MCP_CAP_ALL));

  const std::vector<std::string> names = ListedToolNames();
  ASSERT_EQ(names.size(), 1u);
  EXPECT_EQ(names[0], "ui_tap");
}

// After unregister the provider's surface is gone and its prefix is free, so a
// replacement can claim it.
TEST_F(McpProviderTest, UnregisterReleasesTheNamespace) {
  MockProvider first;
  first.tools.push_back(Tool("tap", IHS_MCP_CAP_INTERACT));
  IhsMcpProvider* provider =
      Register(first.Desc("first", "ui_", "ui", IHS_MCP_CAP_ALL));
  ASSERT_EQ(ListedToolNames().size(), 1u);

  ihs_mcp_provider_unregister(provider);
  registered_.clear();
  EXPECT_TRUE(ListedToolNames().empty());
  EXPECT_EQ(ihs_mcp_provider_count(), 0u);

  MockProvider second;
  second.tools.push_back(Tool("snapshot", IHS_MCP_CAP_INSPECT));
  Register(second.Desc("second", "ui_", "ui", IHS_MCP_CAP_ALL));
  const std::vector<std::string> names = ListedToolNames();
  ASSERT_EQ(names.size(), 1u);
  EXPECT_EQ(names[0], "ui_snapshot");
}

// Releasing is idempotent so an error path can release unconditionally without
// first working out whether anything was produced.
TEST_F(McpProviderTest, PayloadReleaseIsIdempotentAndNullSafe) {
  ihs_mcp_host_release_payload(nullptr);

  IhsMcpPayload payload{};
  ihs_mcp_host_release_payload(&payload);  // zeroed: no release hook

  struct Counter {
    int releases = 0;
  } counter;
  payload.struct_size = sizeof(IhsMcpPayload);
  payload.data = "x";
  payload.length = 1;
  payload.release_ctx = &counter;
  payload.release = [](void* ctx) { static_cast<Counter*>(ctx)->releases++; };

  ihs_mcp_host_release_payload(&payload);
  ihs_mcp_host_release_payload(&payload);
  EXPECT_EQ(counter.releases, 1);
}

TEST_F(McpProviderTest, VisitorsRejectANullCallback) {
  EXPECT_EQ(ihs_mcp_host_for_each_tool(nullptr, nullptr), IHS_MCP_ERR_INVALID);
  EXPECT_EQ(ihs_mcp_host_for_each_resource(nullptr, nullptr),
            IHS_MCP_ERR_INVALID);
}

namespace {

struct SinkRecord {
  std::mutex mutex;
  std::condition_variable arrived;
  int tools_changed = 0;
  int resources_updated = 0;
  std::string last_uri;

  bool WaitFor(const int resource_count) {
    std::unique_lock<std::mutex> lock(mutex);
    return arrived.wait_for(lock, std::chrono::seconds(5), [&] {
      return resources_updated >= resource_count;
    });
  }
};

SinkRecord* g_sink = nullptr;

void OnNotify(IhsMcpNotification kind, const char* uri, void* /*user_data*/) {
  const std::lock_guard<std::mutex> lock(g_sink->mutex);
  if (kind == IHS_MCP_NOTIFY_TOOLS_CHANGED) {
    g_sink->tools_changed++;
  } else {
    g_sink->resources_updated++;
    g_sink->last_uri = uri != nullptr ? uri : "";
  }
  g_sink->arrived.notify_all();
}

}  // namespace

// A provider signalling its notify_fd must reach the sink, which is the only
// route a server has to tell clients anything changed.
TEST_F(McpProviderTest, SignallingTheNotifyFdReachesTheSink) {
  SinkRecord record;
  g_sink = &record;
  ASSERT_EQ(ihs_mcp_host_set_notification_sink(OnNotify, nullptr), IHS_MCP_OK);

  MockProvider mock;
  const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  ASSERT_GE(fd, 0);
  IhsMcpProviderDesc desc = mock.Desc("sema", "ui_", "ui", IHS_MCP_CAP_ALL);
  desc.notify_fd = fd;
  IhsMcpProvider* provider = nullptr;
  ASSERT_EQ(ihs_mcp_provider_register(&desc, &provider), IHS_MCP_OK);

  const uint64_t one = 1;
  ASSERT_EQ(::write(fd, &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));
  EXPECT_TRUE(record.WaitFor(1)) << "sink never saw the signal";
  {
    const std::lock_guard<std::mutex> lock(record.mutex);
    // A provider does not say which changed, so both go out: re-reading an
    // unchanged tool list costs a client nothing, missing a change costs it
    // correctness.
    EXPECT_GE(record.tools_changed, 1);
    EXPECT_EQ(record.last_uri, "ui://");
  }

  ihs_mcp_provider_unregister(provider);
  ASSERT_EQ(ihs_mcp_host_set_notification_sink(nullptr, nullptr), IHS_MCP_OK);
  ::close(fd);
  g_sink = nullptr;
}

// Unregister must not return while the watcher still polls the provider's fd:
// the caller closes it next, and the descriptor number is immediately
// reusable, so the watcher would end up waiting on something unrelated.
TEST_F(McpProviderTest, UnregisterDoesNotLeaveTheWatcherOnAClosedFd) {
  SinkRecord record;
  g_sink = &record;
  ASSERT_EQ(ihs_mcp_host_set_notification_sink(OnNotify, nullptr), IHS_MCP_OK);

  for (int i = 0; i < 20; i++) {
    MockProvider mock;
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_GE(fd, 0);
    IhsMcpProviderDesc desc = mock.Desc("sema", "ui_", "ui", IHS_MCP_CAP_ALL);
    desc.notify_fd = fd;
    IhsMcpProvider* provider = nullptr;
    ASSERT_EQ(ihs_mcp_provider_register(&desc, &provider), IHS_MCP_OK) << i;
    ihs_mcp_provider_unregister(provider);
    // Safe only because unregister waited for the watcher to drop it.
    ::close(fd);
  }

  ASSERT_EQ(ihs_mcp_host_set_notification_sink(nullptr, nullptr), IHS_MCP_OK);
  g_sink = nullptr;
}

// One server per registry: a second sink would mean two things delivering the
// same notifications to different clients.
TEST_F(McpProviderTest, OnlyOneSinkMayBeInstalled) {
  SinkRecord record;
  g_sink = &record;
  ASSERT_EQ(ihs_mcp_host_set_notification_sink(OnNotify, nullptr), IHS_MCP_OK);
  EXPECT_EQ(ihs_mcp_host_set_notification_sink(OnNotify, nullptr),
            IHS_MCP_ERR_REFUSED);
  ASSERT_EQ(ihs_mcp_host_set_notification_sink(nullptr, nullptr), IHS_MCP_OK);
  // Uninstalling twice is harmless, so teardown paths need no bookkeeping.
  EXPECT_EQ(ihs_mcp_host_set_notification_sink(nullptr, nullptr), IHS_MCP_OK);
  g_sink = nullptr;
}
