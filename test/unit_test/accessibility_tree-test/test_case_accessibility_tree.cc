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

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <rapidjson/document.h>

#include "gtest/gtest.h"

#include "accessibility/accessibility_tree.h"

namespace {

// Owns the backing storage (label + child ids) that a FlutterSemanticsNode2's
// pointers reference, so those pointers stay valid for as long as the builder
// lives. The embedder guarantees the same only for the update callback; the
// tree must copy, and these tests verify it does.
struct NodeBuilder {
  FlutterSemanticsNode2 node{};
  std::string label;
  std::string identifier;
  std::vector<int32_t> children;
  std::vector<int32_t> custom_action_ids;
  FlutterSemanticsFlags flags2{};

  NodeBuilder(int32_t id, std::vector<int32_t> kids, std::string lbl) {
    label = std::move(lbl);
    children = std::move(kids);
    node.struct_size = sizeof(FlutterSemanticsNode2);
    node.id = id;
    node.flags__deprecated__ = static_cast<FlutterSemanticsFlag>(0);
    node.actions = static_cast<FlutterSemanticsAction>(0);
    node.label = label.c_str();
    node.hint = "";
    node.value = "";
    node.increased_value = "";
    node.decreased_value = "";
    node.tooltip = "";
    node.identifier = "";
    // Default to the legacy path: the engine only populates flags2 on newer
    // revisions, and Update() must stay correct when it is absent.
    node.flags2 = nullptr;
    node.child_count = children.size();
    node.children_in_traversal_order =
        children.empty() ? nullptr : children.data();
    node.children_in_hit_test_order = node.children_in_traversal_order;
  }

  // Points the node at an owned custom-action ID array, mirroring how the
  // embedder hands over a borrowed array the tree must copy.
  void WithCustomActions(std::vector<int32_t> ids) {
    custom_action_ids = std::move(ids);
    node.custom_accessibility_actions_count = custom_action_ids.size();
    node.custom_accessibility_actions =
        custom_action_ids.empty() ? nullptr : custom_action_ids.data();
  }

  // Points the node at an owned identifier string, as the embedder does.
  void WithIdentifier(std::string id) {
    identifier = std::move(id);
    node.identifier = identifier.c_str();
  }

  // Opts this node into the FlutterSemanticsFlags struct, returning it for
  // field assignment. struct_size is set so the node looks like a newer
  // engine's output.
  FlutterSemanticsFlags& WithFlags2() {
    flags2.struct_size = sizeof(FlutterSemanticsFlags);
    node.flags2 = &flags2;
    return flags2;
  }
};

// Owns the backing strings for a FlutterSemanticsCustomAction2, for the same
// reason NodeBuilder owns the node's: the embedder's pointers are transient.
struct CustomActionBuilder {
  FlutterSemanticsCustomAction2 action{};
  std::string label;
  std::string hint;

  CustomActionBuilder(int32_t id, std::string lbl, std::string hnt) {
    label = std::move(lbl);
    hint = std::move(hnt);
    action.struct_size = sizeof(FlutterSemanticsCustomAction2);
    action.id = id;
    action.override_action = static_cast<FlutterSemanticsAction>(0);
    action.label = label.c_str();
    action.hint = hint.c_str();
  }
};

// Drives HandleFlutterUpdate from a set of already-built nodes and custom
// action declarations.
void Apply(AccessibilityTree& tree,
           std::vector<FlutterSemanticsNode2*> nodes,
           std::vector<FlutterSemanticsCustomAction2*> actions = {}) {
  FlutterSemanticsUpdate2 update{};
  update.struct_size = sizeof(FlutterSemanticsUpdate2);
  update.node_count = nodes.size();
  update.nodes = nodes.data();
  update.custom_action_count = actions.size();
  update.custom_actions = actions.empty() ? nullptr : actions.data();
  tree.HandleFlutterUpdate(&update);
}

AccessibilityNode* FindById(const AccessibilityTree& tree, int32_t id) {
  for (int32_t i = 0; i < tree.NumberOfNodes(); ++i) {
    AccessibilityNode* n = tree.GetNodeByIdx(i);
    if (n != nullptr && n->GetId() == id) {
      return n;
    }
  }
  return nullptr;
}

// Keep DumpTree's first-build write inside the scratch tree, not the real
// user config directory.
class AccessibilityTreeTest : public ::testing::Test {
 protected:
  void SetUp() override {
#ifdef TEST_SCRATCH_DIR
    setenv("XDG_CONFIG_HOME", TEST_SCRATCH_DIR, 1);
#endif
  }
};

}  // namespace

// D2: the node owns its text. Reading a label after the embedder's source
// buffer is freed must be safe (ASan would flag a retained raw pointer).
TEST_F(AccessibilityTreeTest, LabelSurvivesSourceBufferDestruction) {
  auto source = std::make_unique<std::string>("Play button");
  FlutterSemanticsNode2 fl{};
  fl.struct_size = sizeof(FlutterSemanticsNode2);
  fl.id = 0;
  fl.flags__deprecated__ = static_cast<FlutterSemanticsFlag>(0);
  fl.actions = static_cast<FlutterSemanticsAction>(0);
  fl.label = source->c_str();
  fl.hint = "";
  fl.value = "";
  fl.increased_value = "";
  fl.decreased_value = "";
  fl.tooltip = "";

  AccessibilityNode node(fl);
  source.reset();  // free the embedder-owned buffer

  EXPECT_STREQ(node.GetLabel(), "Play button");
}

// D2: a null embedder string coalesces to empty rather than being fed to
// std::string(nullptr) (undefined behavior).
TEST_F(AccessibilityTreeTest, NullStringsCoalesceToEmpty) {
  FlutterSemanticsNode2 fl{};
  fl.struct_size = sizeof(FlutterSemanticsNode2);
  fl.id = 7;
  fl.flags__deprecated__ = static_cast<FlutterSemanticsFlag>(0);
  fl.actions = static_cast<FlutterSemanticsAction>(0);
  fl.label = nullptr;
  fl.hint = nullptr;
  fl.value = nullptr;
  fl.tooltip = nullptr;

  AccessibilityNode node(fl);
  EXPECT_STREQ(node.GetLabel(), "");
  EXPECT_STREQ(node.GetHint(), "");
  EXPECT_STREQ(node.GetValue(), "");
  EXPECT_STREQ(node.GetTooltip(), "");
}

// The tree is only usable once the root (id 0) arrives. Before that, an update
// carrying only non-root nodes leaves the tree unbuilt; pruning is gated behind
// the first build, so an early orphan is retained (not yet dropped) rather than
// pruned against a root that does not exist.
TEST_F(AccessibilityTreeTest, TreeNotBuiltUntilRootArrives) {
  AccessibilityTree tree;
  NodeBuilder orphan(5, {}, "orphan");
  Apply(tree, {&orphan.node});
  EXPECT_FALSE(tree.IsTreeBuilt());
  EXPECT_EQ(tree.NumberOfNodes(), 1);

  NodeBuilder root(0, {5}, "root");
  NodeBuilder child(5, {}, "child");
  Apply(tree, {&root.node, &child.node});
  EXPECT_TRUE(tree.IsTreeBuilt());
  EXPECT_EQ(tree.NumberOfNodes(), 2);
}

// D5: re-sending a node replaces its fields and children rather than appending.
TEST_F(AccessibilityTreeTest, UpdateReplacesLabelAndChildren) {
  AccessibilityTree tree;
  NodeBuilder root1(0, {1}, "before");
  NodeBuilder child(1, {}, "child");
  Apply(tree, {&root1.node, &child.node});
  ASSERT_EQ(tree.NumberOfNodes(), 2);
  ASSERT_STREQ(FindById(tree, 0)->GetLabel(), "before");
  ASSERT_EQ(FindById(tree, 0)->NumberOfChildren(), 1u);

  // Re-send the root with a new label and no children.
  NodeBuilder root2(0, {}, "after");
  Apply(tree, {&root2.node});
  AccessibilityNode* root = FindById(tree, 0);
  ASSERT_NE(root, nullptr);
  EXPECT_STREQ(root->GetLabel(), "after");
  EXPECT_EQ(root->NumberOfChildren(), 0u);
}

// D7: a subtree Flutter detaches is pruned from both the owning vector and the
// index, and the focus falls back to the root if the focused node was dropped.
TEST_F(AccessibilityTreeTest,
       PruneUnreachableDropsDetachedSubtreeAndResetsFocus) {
  AccessibilityTree tree;
  NodeBuilder root1(0, {1}, "root");
  NodeBuilder child(1, {}, "child");
  child.node.flags__deprecated__ = kFlutterSemanticsFlagIsFocused;
  Apply(tree, {&root1.node, &child.node});
  ASSERT_EQ(tree.NumberOfNodes(), 2);
  ASSERT_EQ(tree.GetFocusedNode(), 1);

  // Detach the child by re-sending the root with no children.
  NodeBuilder root2(0, {}, "root");
  Apply(tree, {&root2.node});
  EXPECT_EQ(tree.NumberOfNodes(), 1);
  EXPECT_EQ(FindById(tree, 1), nullptr);
  EXPECT_EQ(tree.GetFocusedNode(), 0);  // focus fell back to root
}

// The action bitmask is retained verbatim, and HasAction() tests single bits
// against it. Without this the node cannot say which verbs a caller may invoke.
TEST_F(AccessibilityTreeTest, RetainsActionBitmask) {
  NodeBuilder slider(0, {}, "Volume");
  slider.node.actions = static_cast<FlutterSemanticsAction>(
      kFlutterSemanticsActionIncrease | kFlutterSemanticsActionDecrease);

  AccessibilityNode node(slider.node);
  EXPECT_EQ(static_cast<uint32_t>(node.GetActions()),
            static_cast<uint32_t>(kFlutterSemanticsActionIncrease |
                                  kFlutterSemanticsActionDecrease));
  EXPECT_TRUE(node.HasAction(kFlutterSemanticsActionIncrease));
  EXPECT_TRUE(node.HasAction(kFlutterSemanticsActionDecrease));
  EXPECT_FALSE(node.HasAction(kFlutterSemanticsActionTap));
}

// HasAction() requires every requested bit, so a caller can test a combination
// without it passing on a partial match.
TEST_F(AccessibilityTreeTest, HasActionRequiresAllRequestedBits) {
  NodeBuilder builder(0, {}, "partial");
  builder.node.actions = kFlutterSemanticsActionIncrease;

  const AccessibilityNode node(builder.node);
  EXPECT_FALSE(node.HasAction(static_cast<FlutterSemanticsAction>(
      kFlutterSemanticsActionIncrease | kFlutterSemanticsActionDecrease)));
}

// The node-to-parent transform is retained: parent-local bounds are not
// comparable across the tree without it.
TEST_F(AccessibilityTreeTest, RetainsTransform) {
  NodeBuilder builder(0, {}, "root");
  builder.node.transform = {2.0, 0.0, 30.0, 0.0, 4.0, 50.0, 0.0, 0.0, 1.0};

  const AccessibilityNode node(builder.node);
  EXPECT_DOUBLE_EQ(node.GetTransform().scaleX, 2.0);
  EXPECT_DOUBLE_EQ(node.GetTransform().scaleY, 4.0);
  EXPECT_DOUBLE_EQ(node.GetTransform().transX, 30.0);
  EXPECT_DOUBLE_EQ(node.GetTransform().transY, 50.0);
  EXPECT_DOUBLE_EQ(node.GetTransform().pers2, 1.0);
}

// A custom action batch is resolved into the tree's declaration table and the
// referencing node keeps the IDs. Previously the batch was iterated and
// discarded, so custom actions were invisible to every consumer.
TEST_F(AccessibilityTreeTest, ResolvesCustomActionsAgainstDeclarations) {
  AccessibilityTree tree;
  NodeBuilder root(0, {}, "root");
  root.WithCustomActions({3, 7});
  CustomActionBuilder sync(3, "Sync zones", "Copies the driver setting");
  CustomActionBuilder reset(7, "Reset", "");
  Apply(tree, {&root.node}, {&sync.action, &reset.action});

  const AccessibilityNode* node = FindById(tree, 0);
  ASSERT_NE(node, nullptr);
  ASSERT_EQ(node->NumberOfCustomActions(), 2u);
  EXPECT_EQ(node->GetCustomActionId(0), 3);
  EXPECT_EQ(node->GetCustomActionId(1), 7);
  EXPECT_EQ(node->GetCustomActionId(2), -1);   // out of bounds
  EXPECT_EQ(node->GetCustomActionId(-1), -1);  // out of bounds

  EXPECT_EQ(tree.NumberOfCustomActions(), 2u);
  const AccessibilityCustomAction* action = tree.FindCustomAction(3);
  ASSERT_NE(action, nullptr);
  EXPECT_EQ(action->id, 3);
  EXPECT_EQ(action->label, "Sync zones");
  EXPECT_EQ(action->hint, "Copies the driver setting");
  EXPECT_EQ(tree.FindCustomAction(99), nullptr);  // never declared
}

// The declaration's strings are owned, like the node text: the embedder frees
// its buffers when the update callback returns.
TEST_F(AccessibilityTreeTest, CustomActionStringsSurviveSourceDestruction) {
  AccessibilityTree tree;
  NodeBuilder root(0, {}, "root");
  {
    CustomActionBuilder sync(3, "Sync zones", "Copies the driver setting");
    Apply(tree, {&root.node}, {&sync.action});
  }  // the embedder's backing strings are gone from here on

  const AccessibilityCustomAction* action = tree.FindCustomAction(3);
  ASSERT_NE(action, nullptr);
  EXPECT_EQ(action->label, "Sync zones");
  EXPECT_EQ(action->hint, "Copies the driver setting");
}

// A null label or hint on a declaration coalesces to empty rather than being
// fed to std::string(nullptr) (undefined behavior), matching the node text.
TEST_F(AccessibilityTreeTest, CustomActionNullStringsCoalesceToEmpty) {
  AccessibilityTree tree;
  NodeBuilder root(0, {}, "root");
  CustomActionBuilder bare(4, "", "");
  bare.action.label = nullptr;
  bare.action.hint = nullptr;
  Apply(tree, {&root.node}, {&bare.action});

  const AccessibilityCustomAction* action = tree.FindCustomAction(4);
  ASSERT_NE(action, nullptr);
  EXPECT_EQ(action->label, "");
  EXPECT_EQ(action->hint, "");
}

// A null entry inside the declaration array is skipped rather than
// dereferenced, and does not abort the rest of the batch.
TEST_F(AccessibilityTreeTest, NullCustomActionEntryIsSkipped) {
  AccessibilityTree tree;
  NodeBuilder root(0, {}, "root");
  CustomActionBuilder sync(3, "Sync zones", "");
  Apply(tree, {&root.node}, {nullptr, &sync.action});

  EXPECT_EQ(tree.NumberOfCustomActions(), 1u);
  ASSERT_NE(tree.FindCustomAction(3), nullptr);
}

// Custom action IDs follow the same replace-don't-append rule as children: an
// action the application withdraws from a node must not linger on it.
TEST_F(AccessibilityTreeTest, UpdateReplacesNodeCustomActions) {
  AccessibilityTree tree;
  NodeBuilder root1(0, {}, "root");
  root1.WithCustomActions({3, 7});
  CustomActionBuilder sync(3, "Sync zones", "");
  CustomActionBuilder reset(7, "Reset", "");
  Apply(tree, {&root1.node}, {&sync.action, &reset.action});
  ASSERT_EQ(FindById(tree, 0)->NumberOfCustomActions(), 2u);

  NodeBuilder root2(0, {}, "root");
  root2.WithCustomActions({7});
  Apply(tree, {&root2.node});
  const AccessibilityNode* node = FindById(tree, 0);
  ASSERT_EQ(node->NumberOfCustomActions(), 1u);
  EXPECT_EQ(node->GetCustomActionId(0), 7);
}

// Declarations deliberately outlive the nodes that reference them: Flutter
// assigns a custom action its ID once and may declare it in an earlier batch
// than the node that uses it, so dropping an unreferenced entry would lose it
// permanently.
TEST_F(AccessibilityTreeTest, CustomActionDeclarationsSurviveNodePruning) {
  AccessibilityTree tree;
  NodeBuilder root1(0, {1}, "root");
  NodeBuilder child(1, {}, "child");
  child.WithCustomActions({3});
  CustomActionBuilder sync(3, "Sync zones", "");
  Apply(tree, {&root1.node, &child.node}, {&sync.action});
  ASSERT_EQ(tree.NumberOfCustomActions(), 1u);

  // Detach the only node that referenced the action.
  NodeBuilder root2(0, {}, "root");
  Apply(tree, {&root2.node});
  ASSERT_EQ(FindById(tree, 1), nullptr);
  EXPECT_EQ(tree.NumberOfCustomActions(), 1u);
  ASSERT_NE(tree.FindCustomAction(3), nullptr);
}

// Re-declaring an ID refreshes the label and hint in place rather than
// accumulating a second entry.
TEST_F(AccessibilityTreeTest, RedeclaringCustomActionReplacesIt) {
  AccessibilityTree tree;
  NodeBuilder root(0, {}, "root");
  CustomActionBuilder before(3, "Sync zones", "old");
  Apply(tree, {&root.node}, {&before.action});
  CustomActionBuilder after(3, "Sync all zones", "new");
  Apply(tree, {&root.node}, {&after.action});

  EXPECT_EQ(tree.NumberOfCustomActions(), 1u);
  const AccessibilityCustomAction* action = tree.FindCustomAction(3);
  ASSERT_NE(action, nullptr);
  EXPECT_EQ(action->label, "Sync all zones");
  EXPECT_EQ(action->hint, "new");
}

// Role and states now come from accessibility::Translate() rather than an
// inline flag check, so the node reports the full derived role.
TEST_F(AccessibilityTreeTest, SpecIsDerivedThroughTheTranslator) {
  NodeBuilder field(5, {}, "Name");
  field.node.flags__deprecated__ = static_cast<FlutterSemanticsFlag>(
      kFlutterSemanticsFlagIsTextField | kFlutterSemanticsFlagHasEnabledState);
  field.node.actions = kFlutterSemanticsActionSetText;

  const AccessibilityNode node(field.node);
  EXPECT_EQ(node.GetSpec().role, accessibility::Role::kTextInput);
  EXPECT_TRUE(node.GetSpec().disabled);  // HasEnabledState && !IsEnabled
  EXPECT_TRUE(node.GetSpec().action_set_text);
  EXPECT_FALSE(node.GetSpec().action_tap);
}

// Translate() distinguishes a label-only leaf from a generic container, so the
// spec must be derived after the label and children have been refreshed.
TEST_F(AccessibilityTreeTest, SpecReflectsRefreshedLabelAndChildren) {
  AccessibilityTree tree;
  NodeBuilder root(0, {1}, "root");
  NodeBuilder leaf(1, {}, "Now playing");
  Apply(tree, {&root.node, &leaf.node});
  ASSERT_EQ(FindById(tree, 1)->GetSpec().role, accessibility::Role::kLabel);

  // Re-send the same node with a child: it is a container now, not a label.
  NodeBuilder branch(1, {2}, "Now playing");
  NodeBuilder grandchild(2, {}, "");
  NodeBuilder root2(0, {1}, "root");
  Apply(tree, {&root2.node, &branch.node, &grandchild.node});
  EXPECT_EQ(FindById(tree, 1)->GetSpec().role,
            accessibility::Role::kGenericContainer);
}

// GetRole() still reports the numeric encoding the AccessKit bridge consumes.
// Only window and button have a SemanticRole constant; every other derived
// role collapses to unknown, which is what this node reported before roles
// were derived through the translator.
TEST_F(AccessibilityTreeTest, GetRoleMapsOnlyWindowAndButton) {
  NodeBuilder root(0, {}, "root");
  EXPECT_EQ(AccessibilityNode(root.node).GetRole(), SEMANTIC_ROLE_WINDOW);

  NodeBuilder button(1, {}, "Play");
  button.node.flags__deprecated__ = kFlutterSemanticsFlagIsButton;
  const AccessibilityNode button_node(button.node);
  EXPECT_EQ(button_node.GetRole(), SEMANTIC_ROLE_BUTTON);
  EXPECT_EQ(button_node.GetSpec().role, accessibility::Role::kButton);

  NodeBuilder slider(2, {}, "Volume");
  slider.node.flags__deprecated__ = kFlutterSemanticsFlagIsSlider;
  const AccessibilityNode slider_node(slider.node);
  EXPECT_EQ(slider_node.GetRole(), SEMANTIC_ROLE_UNKNOWN);
  EXPECT_EQ(slider_node.GetSpec().role, accessibility::Role::kSlider);
}

// The dump is the only way to inspect a running shell's tree, so the retained
// fields have to reach it — a field kept on the node but omitted here is
// invisible in the field.
TEST_F(AccessibilityTreeTest, DumpTreeEmitsActionsTransformAndCustomActions) {
  AccessibilityTree tree;
  NodeBuilder root(0, {}, "root");
  root.node.actions = static_cast<FlutterSemanticsAction>(
      kFlutterSemanticsActionIncrease | kFlutterSemanticsActionDecrease);
  root.node.transform = {2.0, 0.0, 30.0, 0.0, 4.0, 50.0, 0.0, 0.0, 1.0};
  root.WithCustomActions({3});
  root.WithIdentifier("hvac.temp.driver");
  CustomActionBuilder sync(3, "Sync zones", "Copies the driver setting");
  Apply(tree, {&root.node}, {&sync.action});

  const std::string target =
      std::string(TEST_SCRATCH_DIR) + "/dump_tree_enriched.json";
  tree.DumpTree(target.c_str());

  std::ifstream ifs(target);
  ASSERT_TRUE(ifs.is_open());
  const std::string json((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());

  rapidjson::Document doc;
  ASSERT_FALSE(doc.Parse(json.c_str()).HasParseError());
  ASSERT_TRUE(doc.HasMember("nodes"));
  ASSERT_EQ(doc["nodes"].Size(), 1u);
  const rapidjson::Value& node = doc["nodes"][0];

  ASSERT_TRUE(node.HasMember("identifier"));
  EXPECT_STREQ(node["identifier"].GetString(), "hvac.temp.driver");

  // 1 << 6 | 1 << 7 — the increase/decrease bits, emitted as the raw mask.
  ASSERT_TRUE(node.HasMember("actions"));
  EXPECT_EQ(node["actions"].GetInt(), (1 << 6) | (1 << 7));

  ASSERT_TRUE(node.HasMember("transform"));
  EXPECT_DOUBLE_EQ(node["transform"]["scaleX"].GetDouble(), 2.0);
  EXPECT_DOUBLE_EQ(node["transform"]["transY"].GetDouble(), 50.0);

  ASSERT_TRUE(node.HasMember("custom_actions"));
  ASSERT_EQ(node["custom_actions"].Size(), 1u);
  EXPECT_EQ(node["custom_actions"][0].GetInt(), 3);

  // The declarations are emitted once at the top level, not per node.
  ASSERT_TRUE(doc.HasMember("custom_actions"));
  ASSERT_EQ(doc["custom_actions"].Size(), 1u);
  EXPECT_EQ(doc["custom_actions"][0]["id"].GetInt(), 3);
  EXPECT_STREQ(doc["custom_actions"][0]["label"].GetString(), "Sync zones");
  EXPECT_STREQ(doc["custom_actions"][0]["hint"].GetString(),
               "Copies the driver setting");
}

// The application-assigned identifier is retained, owned, and null-safe — it
// is the addressing key that survives tree rebuilds, unlike `id`.
TEST_F(AccessibilityTreeTest, RetainsIdentifier) {
  AccessibilityTree tree;
  NodeBuilder root(0, {}, "root");
  {
    NodeBuilder scoped(0, {}, "root");
    scoped.WithIdentifier("hvac.temp.driver");
    Apply(tree, {&scoped.node});
  }  // the embedder's backing string is gone from here on

  const AccessibilityNode* node = FindById(tree, 0);
  ASSERT_NE(node, nullptr);
  EXPECT_STREQ(node->GetIdentifier(), "hvac.temp.driver");

  // Unannotated and explicitly-null both coalesce to empty, never a null deref.
  NodeBuilder bare(1, {}, "bare");
  EXPECT_STREQ(AccessibilityNode(bare.node).GetIdentifier(), "");
  bare.node.identifier = nullptr;
  EXPECT_STREQ(AccessibilityNode(bare.node).GetIdentifier(), "");
}

// An identifier the application withdraws must not linger on the node.
TEST_F(AccessibilityTreeTest, UpdateReplacesIdentifier) {
  AccessibilityTree tree;
  NodeBuilder before(0, {}, "root");
  before.WithIdentifier("old.id");
  Apply(tree, {&before.node});
  ASSERT_STREQ(FindById(tree, 0)->GetIdentifier(), "old.id");

  NodeBuilder after(0, {}, "root");
  Apply(tree, {&after.node});
  EXPECT_STREQ(FindById(tree, 0)->GetIdentifier(), "");
}

// When the engine supplies FlutterSemanticsFlags it is authoritative, even
// where it disagrees with the deprecated enum bitmask on the same node.
TEST_F(AccessibilityTreeTest, Flags2WinsOverLegacyEnum) {
  NodeBuilder builder(1, {}, "control");
  builder.node.flags__deprecated__ = kFlutterSemanticsFlagIsButton;
  builder.WithFlags2().is_slider = true;

  const AccessibilityNode node(builder.node);
  EXPECT_EQ(node.GetSpec().role, accessibility::Role::kSlider);
}

// Tristates carry information the enum could not: "applies and is false" is
// distinct from "does not apply". A node explicitly reported as not-enabled is
// disabled; one where enablement does not apply is not.
TEST_F(AccessibilityTreeTest, Flags2TristatesDistinguishAbsentFromFalse) {
  NodeBuilder disabled(1, {}, "Save");
  FlutterSemanticsFlags& d = disabled.WithFlags2();
  d.is_button = true;
  d.is_enabled = kFlutterTristateFalse;
  EXPECT_TRUE(AccessibilityNode(disabled.node).GetSpec().disabled);

  NodeBuilder inapplicable(2, {}, "Label");
  FlutterSemanticsFlags& i = inapplicable.WithFlags2();
  i.is_enabled = kFlutterTristateNone;
  EXPECT_FALSE(AccessibilityNode(inapplicable.node).GetSpec().disabled);

  // Mixed check state arrives as its own value rather than a companion bit.
  NodeBuilder mixed(3, {}, "Select all");
  mixed.WithFlags2().is_checked = kFlutterCheckStateMixed;
  const AccessibilityNode mixed_node(mixed.node);
  EXPECT_EQ(mixed_node.GetSpec().role, accessibility::Role::kCheckBox);
  EXPECT_TRUE(mixed_node.GetSpec().checked_mixed);
  EXPECT_FALSE(mixed_node.GetSpec().checked);
}

// A null flags2 (older engine) must still translate, via the deprecated enum.
TEST_F(AccessibilityTreeTest, NullFlags2FallsBackToLegacyEnum) {
  NodeBuilder builder(1, {}, "Play");
  builder.node.flags2 = nullptr;
  builder.node.flags__deprecated__ = kFlutterSemanticsFlagIsButton;

  EXPECT_EQ(AccessibilityNode(builder.node).GetSpec().role,
            accessibility::Role::kButton);
}

// Focus tracking reads the derived spec, so it follows flags2 when present
// rather than only the deprecated bitmask.
TEST_F(AccessibilityTreeTest, FocusTrackedThroughFlags2) {
  AccessibilityTree tree;
  NodeBuilder root(0, {1}, "root");
  NodeBuilder child(1, {}, "child");
  child.WithFlags2().is_focused = kFlutterTristateTrue;
  Apply(tree, {&root.node, &child.node});

  EXPECT_EQ(tree.GetFocusedNode(), 1);
}

// An engine older than this header writes a shorter FlutterSemanticsNode2 and
// reports that length in struct_size. Members appended after that point are
// past the end of its allocation, so they must not be read at all — under ASan
// this test fails outright if the struct_size gate is dropped.
//
// The node is placed at the very end of a heap allocation sized to the *old*
// struct, so any read past it is a genuine out-of-bounds access rather than a
// read of adjacent live memory that happens to look fine.
TEST_F(AccessibilityTreeTest, OlderEngineStructSizeIsNotReadPast) {
  constexpr size_t kOldSize = offsetof(FlutterSemanticsNode2, flags2);
  ASSERT_LT(kOldSize, sizeof(FlutterSemanticsNode2))
      << "flags2/identifier must be appended members for this test to mean "
         "anything";

  auto storage = std::make_unique<unsigned char[]>(kOldSize);
  std::memset(storage.get(), 0, kOldSize);
  auto* fl = reinterpret_cast<FlutterSemanticsNode2*>(storage.get());
  fl->struct_size = kOldSize;  // engine predating flags2 and identifier
  fl->id = 3;
  fl->flags__deprecated__ = kFlutterSemanticsFlagIsButton;
  fl->actions = kFlutterSemanticsActionTap;
  fl->label = "Play";
  fl->hint = "";
  fl->value = "";
  fl->tooltip = "";

  const AccessibilityNode node(*fl);
  EXPECT_EQ(node.GetId(), 3);
  EXPECT_STREQ(node.GetLabel(), "Play");
  EXPECT_STREQ(node.GetIdentifier(), "");  // absent, not garbage
  // Role still derives, via the deprecated enum.
  EXPECT_EQ(node.GetSpec().role, accessibility::Role::kButton);
  EXPECT_TRUE(node.GetSpec().action_tap);
}

// Actions added after the enum's original ceiling are retained like any other.
TEST_F(AccessibilityTreeTest, RetainsActionsAddedAfterLegacyCeiling) {
  NodeBuilder builder(1, {}, "Details");
  builder.node.actions = static_cast<FlutterSemanticsAction>(
      kFlutterSemanticsActionScrollToOffset | kFlutterSemanticsActionExpand |
      kFlutterSemanticsActionCollapse);

  const AccessibilityNode node(builder.node);
  EXPECT_TRUE(node.HasAction(kFlutterSemanticsActionScrollToOffset));
  EXPECT_TRUE(node.GetSpec().action_scroll_to_offset);
  EXPECT_TRUE(node.GetSpec().action_expand);
  EXPECT_TRUE(node.GetSpec().action_collapse);
  EXPECT_FALSE(node.GetSpec().action_tap);
}

// The declarations live in an unordered_map, so the dump sorts them by ID.
// Without that, two dumps of the same tree can differ only in ordering and
// cannot be compared.
TEST_F(AccessibilityTreeTest, DumpTreeEmitsCustomActionsInIdOrder) {
  AccessibilityTree tree;
  NodeBuilder root(0, {}, "root");
  root.WithCustomActions({9, 1, 5});
  CustomActionBuilder c9(9, "nine", "");
  CustomActionBuilder c1(1, "one", "");
  CustomActionBuilder c5(5, "five", "");
  Apply(tree, {&root.node}, {&c9.action, &c1.action, &c5.action});

  const std::string target =
      std::string(TEST_SCRATCH_DIR) + "/dump_tree_action_order.json";
  tree.DumpTree(target.c_str());

  std::ifstream ifs(target);
  ASSERT_TRUE(ifs.is_open());
  const std::string json((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());

  rapidjson::Document doc;
  ASSERT_FALSE(doc.Parse(json.c_str()).HasParseError());
  ASSERT_EQ(doc["custom_actions"].Size(), 3u);
  EXPECT_EQ(doc["custom_actions"][0]["id"].GetInt(), 1);
  EXPECT_EQ(doc["custom_actions"][1]["id"].GetInt(), 5);
  EXPECT_EQ(doc["custom_actions"][2]["id"].GetInt(), 9);

  // The per-node ID list keeps the order the application declared it in.
  ASSERT_EQ(doc["nodes"][0]["custom_actions"].Size(), 3u);
  EXPECT_EQ(doc["nodes"][0]["custom_actions"][0].GetInt(), 9);
  EXPECT_EQ(doc["nodes"][0]["custom_actions"][1].GetInt(), 1);
  EXPECT_EQ(doc["nodes"][0]["custom_actions"][2].GetInt(), 5);
}
