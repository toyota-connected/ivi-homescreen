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

#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "ihs/ihs_semantics.h"
#include "ihs/ihs_semantics_host.h"

namespace {

// Records what the shell would have been asked to do. The hub is supposed to
// have finished validating by the time any of this is called, so the recorded
// calls are also an assertion about what got filtered out earlier.
struct MockHost {
  int enable_calls = 0;
  int disable_calls = 0;
  int dispatch_calls = 0;
  int64_t last_view_id = 0;
  int32_t last_node_id = 0;
  uint64_t last_action = 0;
  std::string last_data;
  int dispatch_result = IHS_SEMANTICS_OK;
};

MockHost* g_host = nullptr;

void OnEnable(void* /*user_data*/, bool enabled) {
  if (enabled) {
    g_host->enable_calls++;
  } else {
    g_host->disable_calls++;
  }
}

int OnDispatch(void* /*user_data*/,
               int64_t view_id,
               int32_t node_id,
               uint64_t action,
               const uint8_t* data,
               size_t data_length) {
  g_host->dispatch_calls++;
  g_host->last_view_id = view_id;
  g_host->last_node_id = node_id;
  g_host->last_action = action;
  g_host->last_data.assign(reinterpret_cast<const char*>(data),
                           data != nullptr ? data_length : 0);
  return g_host->dispatch_result;
}

// The hub is process-global, so each test installs a fresh host and clears any
// tree a previous test published.
class SemanticsHubTest : public ::testing::Test {
 protected:
  void SetUp() override {
    host_ = MockHost{};
    g_host = &host_;
    IhsSemanticsHost host{};
    host.struct_size = sizeof(IhsSemanticsHost);
    host.user_data = nullptr;
    host.set_semantics_enabled = OnEnable;
    host.dispatch = OnDispatch;
    ihs_semantics_set_host(&host);
    ihs_semantics_clear();
  }

  void TearDown() override {
    ihs_semantics_clear();
    ihs_semantics_set_host(nullptr);
    g_host = nullptr;
  }

  MockHost host_;
};

// Builds a publishable tree, owning the arrays the info struct points at.
struct TreeBuilder {
  std::vector<IhsSemanticsPublishNode> nodes;
  std::vector<IhsSemanticsPublishCustomAction> custom_actions;
  std::vector<std::vector<int32_t>> child_storage;
  std::vector<std::vector<int32_t>> custom_storage;

  IhsSemanticsPublishNode& Add(int32_t id, const char* label) {
    child_storage.emplace_back();
    custom_storage.emplace_back();
    IhsSemanticsPublishNode node{};
    node.id = id;
    node.label = label;
    node.role = IHS_SEMANTICS_ROLE_GENERIC_CONTAINER;
    nodes.push_back(node);
    return nodes.back();
  }

  void SetChildren(size_t index, std::vector<int32_t> ids) {
    child_storage[index] = std::move(ids);
    nodes[index].child_ids =
        child_storage[index].empty() ? nullptr : child_storage[index].data();
    nodes[index].child_count = child_storage[index].size();
  }

  void SetCustomActions(size_t index, std::vector<int32_t> ids) {
    custom_storage[index] = std::move(ids);
    nodes[index].custom_action_ids =
        custom_storage[index].empty() ? nullptr : custom_storage[index].data();
    nodes[index].custom_action_count = custom_storage[index].size();
  }

  int Publish() {
    IhsSemanticsPublishInfo info{};
    info.struct_size = sizeof(IhsSemanticsPublishInfo);
    info.nodes = nodes.empty() ? nullptr : nodes.data();
    info.node_count = nodes.size();
    info.custom_actions =
        custom_actions.empty() ? nullptr : custom_actions.data();
    info.custom_action_count = custom_actions.size();
    return ihs_semantics_publish(&info);
  }
};

IhsSemanticsConsumer* Register(const char* name, uint64_t mask, int fd = -1) {
  IhsSemanticsConsumerDesc desc{};
  desc.struct_size = sizeof(IhsSemanticsConsumerDesc);
  desc.name = name;
  desc.action_allow_mask = mask;
  desc.notify_fd = fd;
  IhsSemanticsConsumer* consumer = nullptr;
  EXPECT_EQ(ihs_semantics_register(&desc, &consumer), IHS_SEMANTICS_OK);
  return consumer;
}

}  // namespace

// Nothing is published yet, so there is no tree to acquire. Returning null
// rather than an empty snapshot keeps "semantics never started" distinct from
// "the UI has no nodes".
TEST_F(SemanticsHubTest, AcquireBeforeAnyPublishReturnsNull) {
  EXPECT_EQ(ihs_semantics_acquire_snapshot(), nullptr);
}

TEST_F(SemanticsHubTest, PublishRejectsMalformedInfo) {
  EXPECT_EQ(ihs_semantics_publish(nullptr), IHS_SEMANTICS_ERR_INVALID);
  IhsSemanticsPublishInfo info{};
  info.struct_size = 1;  // not a size this ABI recognizes
  EXPECT_EQ(ihs_semantics_publish(&info), IHS_SEMANTICS_ERR_INVALID);
}

// The shell hands over ids; consumers get indices, resolved once at publish so
// no consumer pays for a lookup per step of a walk.
TEST_F(SemanticsHubTest, PublishResolvesChildIdsToIndices) {
  TreeBuilder tree;
  tree.Add(0, "root");
  tree.Add(7, "child");
  tree.SetChildren(0, {7});
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
  ASSERT_NE(snapshot, nullptr);
  ASSERT_EQ(ihs_semantics_snapshot_node_count(snapshot), 2u);

  const IhsSemanticsNode* root = ihs_semantics_snapshot_node_at(snapshot, 0);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->child_count, 1u);
  EXPECT_EQ(root->child_indices[0], 1u);
  EXPECT_STREQ(
      ihs_semantics_snapshot_node_at(snapshot, root->child_indices[0])->label,
      "child");
  ihs_semantics_release_snapshot(snapshot);
}

// A child id with no node in the batch would be a dangling reference. Dropping
// it keeps every published index dereferenceable.
TEST_F(SemanticsHubTest, PublishDropsChildIdsWithNoNode) {
  TreeBuilder tree;
  tree.Add(0, "root");
  tree.SetChildren(0, {7, 99});  // 99 was never published
  tree.Add(7, "child");
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
  ASSERT_NE(snapshot, nullptr);
  const IhsSemanticsNode* root = ihs_semantics_snapshot_node_at(snapshot, 0);
  ASSERT_EQ(root->child_count, 1u);
  EXPECT_EQ(root->child_indices[0], 1u);
  ihs_semantics_release_snapshot(snapshot);
}

// Consumers are promised non-null strings so they need no null checks; the
// hub is where a null from the shell is turned into "".
TEST_F(SemanticsHubTest, NullStringsBecomeEmptyNotNull) {
  TreeBuilder tree;
  IhsSemanticsPublishNode& node = tree.Add(0, nullptr);
  node.identifier = nullptr;
  node.hint = nullptr;
  node.value = nullptr;
  node.tooltip = nullptr;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
  ASSERT_NE(snapshot, nullptr);
  const IhsSemanticsNode* out = ihs_semantics_snapshot_node_at(snapshot, 0);
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out->identifier, "");
  EXPECT_STREQ(out->label, "");
  EXPECT_STREQ(out->hint, "");
  EXPECT_STREQ(out->value, "");
  EXPECT_STREQ(out->tooltip, "");
  ihs_semantics_release_snapshot(snapshot);
}

// The point of refcounting: a consumer mid-read keeps reading the tree it
// acquired, even as the platform thread publishes over the top of it.
TEST_F(SemanticsHubTest, HeldSnapshotSurvivesLaterPublishes) {
  TreeBuilder first;
  first.Add(0, "before");
  ASSERT_EQ(first.Publish(), IHS_SEMANTICS_OK);

  const IhsSemanticsSnapshot* held = ihs_semantics_acquire_snapshot();
  ASSERT_NE(held, nullptr);
  const uint64_t held_generation = ihs_semantics_snapshot_generation(held);

  for (int i = 0; i < 5; i++) {
    TreeBuilder later;
    later.Add(0, "after");
    ASSERT_EQ(later.Publish(), IHS_SEMANTICS_OK);
  }

  // Still the old tree, and still readable -- ASan would flag it otherwise.
  EXPECT_STREQ(ihs_semantics_snapshot_node_at(held, 0)->label, "before");
  EXPECT_EQ(ihs_semantics_snapshot_generation(held), held_generation);

  const IhsSemanticsSnapshot* latest = ihs_semantics_acquire_snapshot();
  ASSERT_NE(latest, nullptr);
  EXPECT_STREQ(ihs_semantics_snapshot_node_at(latest, 0)->label, "after");
  EXPECT_GT(ihs_semantics_snapshot_generation(latest), held_generation);

  ihs_semantics_release_snapshot(latest);
  ihs_semantics_release_snapshot(held);
}

// Generation orders publications; it does not mark them interesting. A publish
// that changes nothing still advances it, because a consumer's question is
// "is this newer than what I read", not "did something change".
TEST_F(SemanticsHubTest, GenerationAdvancesOnEveryPublish) {
  uint64_t previous = 0;
  for (int i = 0; i < 3; i++) {
    TreeBuilder tree;
    tree.Add(0, "same");
    ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);
    const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
    ASSERT_NE(snapshot, nullptr);
    const uint64_t generation = ihs_semantics_snapshot_generation(snapshot);
    EXPECT_GT(generation, previous);
    previous = generation;
    ihs_semantics_release_snapshot(snapshot);
  }
}

TEST_F(SemanticsHubTest, CustomActionsResolveById) {
  TreeBuilder tree;
  tree.Add(0, "root");
  tree.SetCustomActions(0, {3});
  tree.custom_actions.push_back({3, "Sync zones", "Copies the setting"});
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
  ASSERT_NE(snapshot, nullptr);
  const IhsSemanticsNode* root = ihs_semantics_snapshot_node_at(snapshot, 0);
  ASSERT_EQ(root->custom_action_count, 1u);
  EXPECT_EQ(root->custom_action_ids[0], 3);

  const IhsSemanticsCustomAction* action =
      ihs_semantics_find_custom_action(snapshot, 3);
  ASSERT_NE(action, nullptr);
  EXPECT_STREQ(action->label, "Sync zones");
  EXPECT_EQ(ihs_semantics_find_custom_action(snapshot, 99), nullptr);
  ihs_semantics_release_snapshot(snapshot);
}

TEST_F(SemanticsHubTest, NodeLookupById) {
  TreeBuilder tree;
  tree.Add(0, "root");
  tree.Add(42, "target");
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
  ASSERT_NE(snapshot, nullptr);
  const IhsSemanticsNode* node =
      ihs_semantics_snapshot_node_by_id(snapshot, 42);
  ASSERT_NE(node, nullptr);
  EXPECT_STREQ(node->label, "target");
  EXPECT_EQ(ihs_semantics_snapshot_node_by_id(snapshot, 7), nullptr);
  EXPECT_EQ(ihs_semantics_snapshot_node_at(snapshot, 99), nullptr);
  ihs_semantics_release_snapshot(snapshot);
}

// Semantics costs the engine work, so it is only switched on while somebody is
// listening -- and only on the 0->1 and 1->0 transitions, not per consumer.
TEST_F(SemanticsHubTest, EngineSemanticsFollowsConsumerCount) {
  EXPECT_EQ(host_.enable_calls, 0);

  IhsSemanticsConsumer* first = Register("first", IHS_SEMANTICS_ACTION_ALL);
  EXPECT_EQ(host_.enable_calls, 1);
  EXPECT_EQ(ihs_semantics_consumer_count(), 1u);

  IhsSemanticsConsumer* second = Register("second", IHS_SEMANTICS_ACTION_ALL);
  EXPECT_EQ(host_.enable_calls, 1);  // not re-enabled
  EXPECT_EQ(ihs_semantics_consumer_count(), 2u);

  ihs_semantics_unregister(first);
  EXPECT_EQ(host_.disable_calls, 0);  // one consumer still listening

  ihs_semantics_unregister(second);
  EXPECT_EQ(host_.disable_calls, 1);
  EXPECT_EQ(ihs_semantics_consumer_count(), 0u);
}

TEST_F(SemanticsHubTest, RegisterRejectsMalformedDesc) {
  IhsSemanticsConsumer* consumer = nullptr;
  EXPECT_EQ(ihs_semantics_register(nullptr, &consumer),
            IHS_SEMANTICS_ERR_INVALID);

  IhsSemanticsConsumerDesc desc{};
  desc.struct_size = sizeof(IhsSemanticsConsumerDesc);
  desc.name = nullptr;  // attribution is not optional
  EXPECT_EQ(ihs_semantics_register(&desc, &consumer),
            IHS_SEMANTICS_ERR_INVALID);
  EXPECT_EQ(host_.enable_calls, 0);
}

// The allow mask is the arbitration point between a programmatic driver and a
// screen reader sharing one tree, so a denial must never reach the shell.
TEST_F(SemanticsHubTest, DeniedActionNeverReachesTheHost) {
  TreeBuilder tree;
  IhsSemanticsPublishNode& node = tree.Add(0, "root");
  node.actions = IHS_SEMANTICS_ACTION_ALL;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  IhsSemanticsConsumer* agent =
      Register("agent", IHS_SEMANTICS_ACTION_NO_A11Y_FOCUS);

  EXPECT_EQ(ihs_semantics_dispatch(agent, 0, 0,
                                   IHS_SEMANTICS_ACTION_DID_GAIN_A11Y_FOCUS,
                                   nullptr, 0, nullptr, nullptr),
            IHS_SEMANTICS_ERR_DENIED);
  EXPECT_EQ(host_.dispatch_calls, 0);

  // An action inside the mask goes through.
  EXPECT_EQ(ihs_semantics_dispatch(agent, 0, 0, IHS_SEMANTICS_ACTION_TAP,
                                   nullptr, 0, nullptr, nullptr),
            IHS_SEMANTICS_OK);
  EXPECT_EQ(host_.dispatch_calls, 1);
  ihs_semantics_unregister(agent);
}

// A denial must not depend on whether the node exists, or a caller could probe
// the tree with actions it is not allowed to invoke and read the difference
// between "denied" and "no such node".
TEST_F(SemanticsHubTest, DenialIsCheckedBeforeNodeExistence) {
  TreeBuilder tree;
  tree.Add(0, "root");
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  IhsSemanticsConsumer* agent =
      Register("agent", IHS_SEMANTICS_ACTION_NO_A11Y_FOCUS);
  EXPECT_EQ(ihs_semantics_dispatch(agent, 0, 12345,
                                   IHS_SEMANTICS_ACTION_DID_GAIN_A11Y_FOCUS,
                                   nullptr, 0, nullptr, nullptr),
            IHS_SEMANTICS_ERR_DENIED);
  ihs_semantics_unregister(agent);
}

// The framework silently drops an action the widget registered no handler for,
// so the hub refuses it here and the caller learns something instead.
TEST_F(SemanticsHubTest, ActionNotOfferedByNodeIsRefused) {
  TreeBuilder tree;
  IhsSemanticsPublishNode& node = tree.Add(0, "root");
  node.actions = IHS_SEMANTICS_ACTION_TAP;  // tap only
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  IhsSemanticsConsumer* agent = Register("agent", IHS_SEMANTICS_ACTION_ALL);
  EXPECT_EQ(ihs_semantics_dispatch(agent, 0, 0, IHS_SEMANTICS_ACTION_INCREASE,
                                   nullptr, 0, nullptr, nullptr),
            IHS_SEMANTICS_ERR_UNSUPPORTED_ACTION);
  EXPECT_EQ(host_.dispatch_calls, 0);

  EXPECT_EQ(ihs_semantics_dispatch(agent, 0, 99, IHS_SEMANTICS_ACTION_TAP,
                                   nullptr, 0, nullptr, nullptr),
            IHS_SEMANTICS_ERR_NO_SUCH_NODE);
  EXPECT_EQ(host_.dispatch_calls, 0);
  ihs_semantics_unregister(agent);
}

TEST_F(SemanticsHubTest, DispatchForwardsArgumentsAndPayload) {
  TreeBuilder tree;
  IhsSemanticsPublishNode& node = tree.Add(12, "field");
  node.actions = IHS_SEMANTICS_ACTION_SET_TEXT;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  IhsSemanticsConsumer* agent = Register("agent", IHS_SEMANTICS_ACTION_ALL);
  const std::string payload = "hello";
  EXPECT_EQ(
      ihs_semantics_dispatch(agent, 3, 12, IHS_SEMANTICS_ACTION_SET_TEXT,
                             reinterpret_cast<const uint8_t*>(payload.data()),
                             payload.size(), nullptr, nullptr),
      IHS_SEMANTICS_OK);
  EXPECT_EQ(host_.last_view_id, 3);
  EXPECT_EQ(host_.last_node_id, 12);
  EXPECT_EQ(host_.last_action, IHS_SEMANTICS_ACTION_SET_TEXT);
  EXPECT_EQ(host_.last_data, payload);
  ihs_semantics_unregister(agent);
}

TEST_F(SemanticsHubTest, DoneCallbackReceivesTheStatus) {
  TreeBuilder tree;
  IhsSemanticsPublishNode& node = tree.Add(0, "root");
  node.actions = IHS_SEMANTICS_ACTION_TAP;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  IhsSemanticsConsumer* agent = Register("agent", IHS_SEMANTICS_ACTION_ALL);

  struct Capture {
    int status = 1234;
    int calls = 0;
  } capture;
  auto done = [](int status, void* user_data) {
    auto* c = static_cast<Capture*>(user_data);
    c->status = status;
    c->calls++;
  };

  EXPECT_EQ(ihs_semantics_dispatch(agent, 0, 0, IHS_SEMANTICS_ACTION_TAP,
                                   nullptr, 0, done, &capture),
            IHS_SEMANTICS_OK);
  EXPECT_EQ(capture.calls, 1);
  EXPECT_EQ(capture.status, IHS_SEMANTICS_OK);

  // A refusal reports through the same callback rather than silently not
  // firing, so a caller can drive everything off one path.
  EXPECT_EQ(ihs_semantics_dispatch(agent, 0, 0, IHS_SEMANTICS_ACTION_INCREASE,
                                   nullptr, 0, done, &capture),
            IHS_SEMANTICS_ERR_UNSUPPORTED_ACTION);
  EXPECT_EQ(capture.calls, 2);
  EXPECT_EQ(capture.status, IHS_SEMANTICS_ERR_UNSUPPORTED_ACTION);
  ihs_semantics_unregister(agent);
}

// Dispatching through a handle that has been unregistered must not reach the
// shell -- the consumer is gone and its allow mask with it.
TEST_F(SemanticsHubTest, DispatchAfterUnregisterIsRejected) {
  TreeBuilder tree;
  IhsSemanticsPublishNode& node = tree.Add(0, "root");
  node.actions = IHS_SEMANTICS_ACTION_ALL;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  IhsSemanticsConsumer* agent = Register("agent", IHS_SEMANTICS_ACTION_ALL);
  ihs_semantics_unregister(agent);

  EXPECT_EQ(ihs_semantics_dispatch(agent, 0, 0, IHS_SEMANTICS_ACTION_TAP,
                                   nullptr, 0, nullptr, nullptr),
            IHS_SEMANTICS_ERR_INVALID);
  EXPECT_EQ(host_.dispatch_calls, 0);
}

// Without a shell there is nothing to dispatch to. Saying so beats pretending
// the action was delivered.
TEST_F(SemanticsHubTest, DispatchWithoutHostIsUnavailable) {
  TreeBuilder tree;
  IhsSemanticsPublishNode& node = tree.Add(0, "root");
  node.actions = IHS_SEMANTICS_ACTION_ALL;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  IhsSemanticsConsumer* agent = Register("agent", IHS_SEMANTICS_ACTION_ALL);
  ihs_semantics_set_host(nullptr);

  EXPECT_EQ(ihs_semantics_dispatch(agent, 0, 0, IHS_SEMANTICS_ACTION_TAP,
                                   nullptr, 0, nullptr, nullptr),
            IHS_SEMANTICS_ERR_UNAVAILABLE);
  ihs_semantics_unregister(agent);
}

// A failure the shell reports is passed back verbatim rather than being
// flattened into a generic error.
TEST_F(SemanticsHubTest, HostFailureIsReportedToTheCaller) {
  TreeBuilder tree;
  IhsSemanticsPublishNode& node = tree.Add(0, "root");
  node.actions = IHS_SEMANTICS_ACTION_ALL;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  host_.dispatch_result = IHS_SEMANTICS_ERR_UNAVAILABLE;
  IhsSemanticsConsumer* agent = Register("agent", IHS_SEMANTICS_ACTION_ALL);
  EXPECT_EQ(ihs_semantics_dispatch(agent, 0, 0, IHS_SEMANTICS_ACTION_TAP,
                                   nullptr, 0, nullptr, nullptr),
            IHS_SEMANTICS_ERR_UNAVAILABLE);
  EXPECT_EQ(host_.dispatch_calls, 1);
  ihs_semantics_unregister(agent);
}

// Publishing writes the consumer's fd so it can wake in its own event loop.
TEST_F(SemanticsHubTest, PublishSignalsTheNotifyFd) {
  int fds[2];
  ASSERT_EQ(::pipe(fds), 0);

  IhsSemanticsConsumer* consumer =
      Register("watcher", IHS_SEMANTICS_ACTION_ALL, fds[1]);

  TreeBuilder tree;
  tree.Add(0, "root");
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  uint64_t token = 0;
  EXPECT_GT(::read(fds[0], &token, sizeof(token)), 0);

  ihs_semantics_unregister(consumer);
  ::close(fds[0]);
  ::close(fds[1]);
}

// A consumer that opts out of notification (fd -1) must not break publish for
// everyone else.
TEST_F(SemanticsHubTest, ConsumerWithoutNotifyFdIsHarmless) {
  IhsSemanticsConsumer* quiet = Register("quiet", IHS_SEMANTICS_ACTION_ALL, -1);
  TreeBuilder tree;
  tree.Add(0, "root");
  EXPECT_EQ(tree.Publish(), IHS_SEMANTICS_OK);
  ihs_semantics_unregister(quiet);
}

// Concurrent readers against a publisher: the refcount protocol is the whole
// reason a consumer can read off the platform thread, so exercise it under
// TSan/ASan rather than trusting it by inspection.
TEST_F(SemanticsHubTest, ConcurrentAcquireDuringPublish) {
  TreeBuilder seed;
  seed.Add(0, "seed");
  ASSERT_EQ(seed.Publish(), IHS_SEMANTICS_OK);

  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int i = 0; i < 4; i++) {
    readers.emplace_back([&stop]() {
      while (!stop.load(std::memory_order_relaxed)) {
        const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
        if (snapshot != nullptr) {
          // Touch the contents: a use-after-free shows up here, not at acquire.
          const IhsSemanticsNode* node =
              ihs_semantics_snapshot_node_at(snapshot, 0);
          if (node != nullptr) {
            volatile size_t len = std::strlen(node->label);
            (void)len;
          }
          ihs_semantics_release_snapshot(snapshot);
        }
      }
    });
  }

  for (int i = 0; i < 200; i++) {
    TreeBuilder tree;
    tree.Add(0, "churn");
    ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);
  }

  stop.store(true, std::memory_order_relaxed);
  for (auto& reader : readers) {
    reader.join();
  }
}

// The numeric value a publisher supplies reaches consumers intact, and the
// accessor is the read path a plugin built against a newer header uses.
TEST_F(SemanticsHubTest, NumericValueReachesConsumers) {
  TreeBuilder tree;
  IhsSemanticsPublishNode& node = tree.Add(0, "Media list");
  node.has_numeric_value = true;
  node.numeric_value = 120.0;
  node.numeric_value_min = 0.0;
  node.numeric_value_max = 400.0;
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
  ASSERT_NE(snapshot, nullptr);
  const IhsSemanticsNode* out = ihs_semantics_snapshot_node_at(snapshot, 0);
  ASSERT_NE(out, nullptr);

  double value = -1.0;
  double min = -1.0;
  double max = -1.0;
  EXPECT_TRUE(ihs_semantics_node_numeric_value(out, &value, &min, &max));
  EXPECT_DOUBLE_EQ(value, 120.0);
  EXPECT_DOUBLE_EQ(min, 0.0);
  EXPECT_DOUBLE_EQ(max, 400.0);
  ihs_semantics_release_snapshot(snapshot);
}

// Consumers are told the bounds are finite and ordered and may pass them
// straight to a platform accessibility API, so the hub refuses a range it
// cannot stand behind rather than forwarding it and trusting every consumer to
// re-check. A NaN or an inverted range reaching AT-SPI is a crash in someone
// else's process.
TEST_F(SemanticsHubTest, ImplausibleNumericValueIsDropped) {
  const double nan_value = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();

  struct Case {
    const char* name;
    double value;
    double min;
    double max;
  };
  const Case cases[] = {
      {"inverted range", 5.0, 10.0, 0.0},
      {"value below min", -1.0, 0.0, 10.0},
      {"value above max", 11.0, 0.0, 10.0},
      {"nan value", nan_value, 0.0, 10.0},
      {"nan bound", 5.0, nan_value, 10.0},
      {"infinite bound", 5.0, 0.0, infinity},
  };

  for (const Case& c : cases) {
    TreeBuilder tree;
    IhsSemanticsPublishNode& node = tree.Add(0, c.name);
    node.has_numeric_value = true;
    node.numeric_value = c.value;
    node.numeric_value_min = c.min;
    node.numeric_value_max = c.max;
    ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK) << c.name;

    const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
    ASSERT_NE(snapshot, nullptr) << c.name;
    const IhsSemanticsNode* out = ihs_semantics_snapshot_node_at(snapshot, 0);
    ASSERT_NE(out, nullptr) << c.name;
    EXPECT_FALSE(out->has_numeric_value) << c.name;
    EXPECT_DOUBLE_EQ(out->numeric_value, 0.0) << c.name;
    ihs_semantics_release_snapshot(snapshot);
  }
}

// The accessor leaves the caller's variables alone when there is no value, so
// a caller that ignores the return does not read an uninitialized position as
// if it were real.
TEST_F(SemanticsHubTest, NumericValueAccessorLeavesOutputsAloneWhenAbsent) {
  TreeBuilder tree;
  tree.Add(0, "plain");
  ASSERT_EQ(tree.Publish(), IHS_SEMANTICS_OK);

  const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
  ASSERT_NE(snapshot, nullptr);
  const IhsSemanticsNode* out = ihs_semantics_snapshot_node_at(snapshot, 0);
  ASSERT_NE(out, nullptr);

  double value = 42.0;
  double min = 43.0;
  double max = 44.0;
  EXPECT_FALSE(ihs_semantics_node_numeric_value(out, &value, &min, &max));
  EXPECT_DOUBLE_EQ(value, 42.0);
  EXPECT_DOUBLE_EQ(min, 43.0);
  EXPECT_DOUBLE_EQ(max, 44.0);

  // Null outputs are legal for a caller that only wants the presence test,
  // and a null node is not a crash.
  EXPECT_FALSE(
      ihs_semantics_node_numeric_value(out, nullptr, nullptr, nullptr));
  EXPECT_FALSE(ihs_semantics_node_numeric_value(nullptr, &value, &min, &max));
  ihs_semantics_release_snapshot(snapshot);
}

namespace {

int g_taps = 0;
double g_tap_x = 0.0;
double g_tap_y = 0.0;

int RecordTap(void*, int64_t, double x, double y) {
  g_taps++;
  g_tap_x = x;
  g_tap_y = y;
  return IHS_SEMANTICS_OK;
}

}  // namespace

// The pointer fallback reaches the shell with the coordinates it was given.
TEST_F(SemanticsHubTest, PointerTapReachesTheHost) {
  g_taps = 0;
  IhsSemanticsHost host{};
  host.struct_size = sizeof(host);
  host.send_pointer_tap = RecordTap;
  ihs_semantics_set_host(&host);

  IhsSemanticsConsumer* consumer = Register("agent", IHS_SEMANTICS_ACTION_TAP);
  ASSERT_NE(consumer, nullptr);
  EXPECT_EQ(ihs_semantics_send_pointer_tap(consumer, 0, 120.5, 48.0),
            IHS_SEMANTICS_OK);
  EXPECT_EQ(g_taps, 1);
  EXPECT_DOUBLE_EQ(g_tap_x, 120.5);
  EXPECT_DOUBLE_EQ(g_tap_y, 48.0);
  ihs_semantics_unregister(consumer);
}

// The one that matters: a synthesized tap is a tap, so a consumer denied the
// action must not obtain it by coordinate instead. Without this the allow
// mask would arbitrate nothing -- anything it refused could be performed by
// naming a point rather than a node.
TEST_F(SemanticsHubTest, PointerTapIsRefusedWithoutTapPermission) {
  g_taps = 0;
  IhsSemanticsHost host{};
  host.struct_size = sizeof(host);
  host.send_pointer_tap = RecordTap;
  ihs_semantics_set_host(&host);

  // Everything except tap.
  IhsSemanticsConsumer* consumer = Register(
      "observer", IHS_SEMANTICS_ACTION_ALL & ~IHS_SEMANTICS_ACTION_TAP);
  ASSERT_NE(consumer, nullptr);
  EXPECT_EQ(ihs_semantics_send_pointer_tap(consumer, 0, 1.0, 1.0),
            IHS_SEMANTICS_ERR_DENIED);
  EXPECT_EQ(g_taps, 0) << "a denied tap still reached the shell";
  ihs_semantics_unregister(consumer);
}

// A shell with no input path says so rather than reporting a tap it never
// delivered.
TEST_F(SemanticsHubTest, PointerTapIsUnsupportedWithoutAHostHook) {
  IhsSemanticsHost host{};
  host.struct_size = sizeof(host);
  host.send_pointer_tap = nullptr;
  ihs_semantics_set_host(&host);

  IhsSemanticsConsumer* consumer = Register("agent", IHS_SEMANTICS_ACTION_TAP);
  ASSERT_NE(consumer, nullptr);
  EXPECT_EQ(ihs_semantics_send_pointer_tap(consumer, 0, 1.0, 1.0),
            IHS_SEMANTICS_ERR_UNSUPPORTED_ACTION);
  ihs_semantics_unregister(consumer);
}

// A coordinate that is not a number would land somewhere unpredictable.
TEST_F(SemanticsHubTest, NonFinitePointerCoordinatesAreRefused) {
  g_taps = 0;
  IhsSemanticsHost host{};
  host.struct_size = sizeof(host);
  host.send_pointer_tap = RecordTap;
  ihs_semantics_set_host(&host);

  IhsSemanticsConsumer* consumer = Register("agent", IHS_SEMANTICS_ACTION_TAP);
  ASSERT_NE(consumer, nullptr);
  const double nan_value = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  EXPECT_EQ(ihs_semantics_send_pointer_tap(consumer, 0, nan_value, 1.0),
            IHS_SEMANTICS_ERR_INVALID);
  EXPECT_EQ(ihs_semantics_send_pointer_tap(consumer, 0, 1.0, infinity),
            IHS_SEMANTICS_ERR_INVALID);
  EXPECT_EQ(g_taps, 0);
  ihs_semantics_unregister(consumer);
}

// An unregistered consumer is refused, as with dispatch: the handle is how
// the funnel knows who is asking, and attribution is the point of it.
TEST_F(SemanticsHubTest, PointerTapFromAnUnknownConsumerIsRefused) {
  IhsSemanticsHost host{};
  host.struct_size = sizeof(host);
  host.send_pointer_tap = RecordTap;
  ihs_semantics_set_host(&host);
  EXPECT_EQ(ihs_semantics_send_pointer_tap(nullptr, 0, 1.0, 1.0),
            IHS_SEMANTICS_ERR_INVALID);
}
