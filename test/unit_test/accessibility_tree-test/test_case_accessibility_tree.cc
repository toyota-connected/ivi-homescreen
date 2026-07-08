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

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

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
  std::vector<int32_t> children;

  NodeBuilder(int32_t id, std::vector<int32_t> kids, std::string lbl) {
    label = std::move(lbl);
    children = std::move(kids);
    node.struct_size = sizeof(FlutterSemanticsNode2);
    node.id = id;
    node.flags = static_cast<FlutterSemanticsFlag>(0);
    node.actions = static_cast<FlutterSemanticsAction>(0);
    node.label = label.c_str();
    node.hint = "";
    node.value = "";
    node.increased_value = "";
    node.decreased_value = "";
    node.tooltip = "";
    node.child_count = children.size();
    node.children_in_traversal_order =
        children.empty() ? nullptr : children.data();
    node.children_in_hit_test_order = node.children_in_traversal_order;
  }
};

// Drives HandleFlutterUpdate from a set of already-built nodes.
void Apply(AccessibilityTree& tree, std::vector<FlutterSemanticsNode2*> nodes) {
  FlutterSemanticsUpdate2 update{};
  update.struct_size = sizeof(FlutterSemanticsUpdate2);
  update.node_count = nodes.size();
  update.nodes = nodes.data();
  update.custom_action_count = 0;
  update.custom_actions = nullptr;
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
  fl.flags = static_cast<FlutterSemanticsFlag>(0);
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
  fl.flags = static_cast<FlutterSemanticsFlag>(0);
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
  child.node.flags = kFlutterSemanticsFlagIsFocused;
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
