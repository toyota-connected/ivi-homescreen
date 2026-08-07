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
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "ihs/ihs_semantics.h"
#include "ihs/ihs_semantics_host.h"

#include "accessibility/accessibility_tree.h"
#include "accessibility/semantics_publisher.h"

using accessibility::Affine;
using accessibility::Concat;
using accessibility::FromFlutter;
using accessibility::MapRect;
using accessibility::PublishTree;
using accessibility::Role;
using accessibility::ToFlutterSemanticsAction;
using accessibility::ToIhsRole;

namespace {

// Mirrors the accessibility_tree tests: owns what the node's pointers
// reference, since the embedder's own buffers are transient.
struct NodeBuilder {
  FlutterSemanticsNode2 node{};
  std::string label;
  std::vector<int32_t> children;

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
    node.tooltip = "";
    node.identifier = "";
    node.flags2 = nullptr;
    node.transform = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    node.child_count = children.size();
    node.children_in_traversal_order =
        children.empty() ? nullptr : children.data();
    node.children_in_hit_test_order = node.children_in_traversal_order;
  }
};

void Apply(AccessibilityTree& tree, std::vector<FlutterSemanticsNode2*> nodes) {
  FlutterSemanticsUpdate2 update{};
  update.struct_size = sizeof(FlutterSemanticsUpdate2);
  update.node_count = nodes.size();
  update.nodes = nodes.data();
  update.custom_action_count = 0;
  update.custom_actions = nullptr;
  tree.HandleFlutterUpdate(&update);
}

class SemanticsPublisherTest : public ::testing::Test {
 protected:
  void SetUp() override {
#ifdef TEST_SCRATCH_DIR
    setenv("XDG_CONFIG_HOME", TEST_SCRATCH_DIR, 1);
#endif
    ihs_semantics_clear();
  }
  void TearDown() override { ihs_semantics_clear(); }
};

}  // namespace

// Composition order matters: the parent-to-screen transform is applied after
// the child's node-to-parent one, not before. Getting it backwards puts every
// nested node in the wrong place, which is the kind of thing that looks fine
// at the root and is wrong everywhere else.
TEST_F(SemanticsPublisherTest, ConcatAppliesOuterAfterInner) {
  Affine translate;
  translate.e = 10.0;
  translate.f = 20.0;

  Affine scale;
  scale.a = 2.0;
  scale.d = 2.0;

  // Scale applied after translate: the translation is scaled too.
  const Affine scaled_translate = Concat(scale, translate);
  EXPECT_DOUBLE_EQ(scaled_translate.e, 20.0);
  EXPECT_DOUBLE_EQ(scaled_translate.f, 40.0);

  // Translate applied after scale: the translation is not scaled.
  const Affine translated_scale = Concat(translate, scale);
  EXPECT_DOUBLE_EQ(translated_scale.e, 10.0);
  EXPECT_DOUBLE_EQ(translated_scale.f, 20.0);
}

TEST_F(SemanticsPublisherTest, FromFlutterReadsTheAffinePart) {
  const FlutterTransformation t = {2.0,  3.0, 30.0, 4.0, 5.0,
                                   50.0, 0.0, 0.0,  1.0};
  const Affine a = FromFlutter(t);
  EXPECT_DOUBLE_EQ(a.a, 2.0);   // scaleX
  EXPECT_DOUBLE_EQ(a.c, 3.0);   // skewX
  EXPECT_DOUBLE_EQ(a.e, 30.0);  // transX
  EXPECT_DOUBLE_EQ(a.b, 4.0);   // skewY
  EXPECT_DOUBLE_EQ(a.d, 5.0);   // scaleY
  EXPECT_DOUBLE_EQ(a.f, 50.0);  // transY
}

TEST_F(SemanticsPublisherTest, MapRectHandlesTranslationAndScale) {
  Affine t;
  t.a = 2.0;
  t.d = 3.0;
  t.e = 5.0;
  t.f = 7.0;

  const FlutterRect rect = {0.0, 0.0, 10.0, 10.0};
  const IhsSemanticsRect out = MapRect(t, rect);
  EXPECT_DOUBLE_EQ(out.left, 5.0);
  EXPECT_DOUBLE_EQ(out.top, 7.0);
  EXPECT_DOUBLE_EQ(out.right, 25.0);
  EXPECT_DOUBLE_EQ(out.bottom, 37.0);
}

// Under rotation the transformed corners are not the transforms of the
// original min/max, so mapping only two corners silently produces a wrong box.
TEST_F(SemanticsPublisherTest, MapRectBoundsAllFourCornersUnderRotation) {
  // 90 degree rotation: (x, y) -> (-y, x).
  Affine rotate;
  rotate.a = 0.0;
  rotate.b = 1.0;
  rotate.c = -1.0;
  rotate.d = 0.0;

  const FlutterRect rect = {0.0, 0.0, 10.0, 4.0};
  const IhsSemanticsRect out = MapRect(rotate, rect);
  EXPECT_DOUBLE_EQ(out.left, -4.0);
  EXPECT_DOUBLE_EQ(out.top, 0.0);
  EXPECT_DOUBLE_EQ(out.right, 0.0);
  EXPECT_DOUBLE_EQ(out.bottom, 10.0);
}

// Every hub action must map back to a framework action, or dispatch of that
// verb is silently impossible.
TEST_F(SemanticsPublisherTest, EveryHubActionMapsToAFrameworkAction) {
  for (int bit = 0; bit < 64; bit++) {
    const uint64_t value = UINT64_C(1) << bit;
    if ((IHS_SEMANTICS_ACTION_ALL & value) == 0) {
      continue;
    }
    EXPECT_NE(ToFlutterSemanticsAction(value),
              static_cast<FlutterSemanticsAction>(0))
        << "hub action bit " << bit << " has no framework equivalent";
  }
}

// The framework performs one action at a time. Honoring part of a combined
// request would be worse than refusing all of it.
TEST_F(SemanticsPublisherTest, MultiBitAndUnknownActionsAreRefused) {
  EXPECT_EQ(ToFlutterSemanticsAction(0),
            static_cast<FlutterSemanticsAction>(0));
  EXPECT_EQ(ToFlutterSemanticsAction(IHS_SEMANTICS_ACTION_TAP |
                                     IHS_SEMANTICS_ACTION_INCREASE),
            static_cast<FlutterSemanticsAction>(0));
  // A bit outside the defined set.
  EXPECT_EQ(ToFlutterSemanticsAction(UINT64_C(1) << 40),
            static_cast<FlutterSemanticsAction>(0));
}

TEST_F(SemanticsPublisherTest, RolesMapOntoTheHubEnum) {
  EXPECT_EQ(ToIhsRole(Role::kWindow), IHS_SEMANTICS_ROLE_WINDOW);
  EXPECT_EQ(ToIhsRole(Role::kSlider), IHS_SEMANTICS_ROLE_SLIDER);
  EXPECT_EQ(ToIhsRole(Role::kPasswordInput), IHS_SEMANTICS_ROLE_PASSWORD_INPUT);
  EXPECT_EQ(ToIhsRole(Role::kUnknown), IHS_SEMANTICS_ROLE_UNKNOWN);
}

// Nothing to publish before the root arrives, and saying so is not an error.
TEST_F(SemanticsPublisherTest, PublishWithNoRootIsANoOp) {
  AccessibilityTree tree;
  NodeBuilder orphan(5, {}, "orphan");
  Apply(tree, {&orphan.node});

  EXPECT_EQ(PublishTree(tree), IHS_SEMANTICS_OK);
  EXPECT_EQ(ihs_semantics_acquire_snapshot(), nullptr);
}

// The composed transform is what makes rects comparable across the tree: a
// child's screen rect must include its ancestors' translations.
TEST_F(SemanticsPublisherTest, ChildRectsAreComposedToScreenSpace) {
  AccessibilityTree tree;
  NodeBuilder root(0, {1}, "root");
  root.node.transform = {1.0, 0.0, 100.0, 0.0, 1.0, 200.0, 0.0, 0.0, 1.0};
  NodeBuilder child(1, {}, "child");
  child.node.transform = {1.0, 0.0, 10.0, 0.0, 1.0, 20.0, 0.0, 0.0, 1.0};
  child.node.rect = {0.0, 0.0, 5.0, 5.0};
  Apply(tree, {&root.node, &child.node});

  ASSERT_EQ(PublishTree(tree), IHS_SEMANTICS_OK);
  const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
  ASSERT_NE(snapshot, nullptr);

  const IhsSemanticsNode* out = ihs_semantics_snapshot_node_by_id(snapshot, 1);
  ASSERT_NE(out, nullptr);
  // 100 + 10, 200 + 20 -- both ancestors' translations applied.
  EXPECT_DOUBLE_EQ(out->rect.left, 110.0);
  EXPECT_DOUBLE_EQ(out->rect.top, 220.0);
  EXPECT_DOUBLE_EQ(out->rect.right, 115.0);
  EXPECT_DOUBLE_EQ(out->rect.bottom, 225.0);
  ihs_semantics_release_snapshot(snapshot);
}

// The hub promises index 0 is the root and that every child index resolves.
TEST_F(SemanticsPublisherTest, RootIsPublishedFirstInTraversalOrder) {
  AccessibilityTree tree;
  NodeBuilder root(0, {1, 2}, "root");
  NodeBuilder first(1, {}, "first");
  NodeBuilder second(2, {}, "second");
  // Deliberately not root-first in the update.
  Apply(tree, {&second.node, &first.node, &root.node});

  ASSERT_EQ(PublishTree(tree), IHS_SEMANTICS_OK);
  const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
  ASSERT_NE(snapshot, nullptr);

  ASSERT_EQ(ihs_semantics_snapshot_node_count(snapshot), 3u);
  EXPECT_EQ(ihs_semantics_snapshot_node_at(snapshot, 0)->id, 0);
  EXPECT_STREQ(ihs_semantics_snapshot_node_at(snapshot, 1)->label, "first");
  EXPECT_STREQ(ihs_semantics_snapshot_node_at(snapshot, 2)->label, "second");
  ihs_semantics_release_snapshot(snapshot);
}

// Tristates must survive the trip: a consumer reading "enabled: false" needs
// to be able to tell a disabled control from one where enablement is
// meaningless, which is the whole reason the hub carries three values.
TEST_F(SemanticsPublisherTest, TristatesArePublishedNotFlattened) {
  AccessibilityTree tree;
  NodeBuilder root(0, {1, 2}, "root");

  NodeBuilder disabled(1, {}, "Save");
  disabled.node.flags__deprecated__ = static_cast<FlutterSemanticsFlag>(
      kFlutterSemanticsFlagIsButton | kFlutterSemanticsFlagHasEnabledState);

  NodeBuilder plain(2, {}, "Label");  // enablement does not apply

  Apply(tree, {&root.node, &disabled.node, &plain.node});
  ASSERT_EQ(PublishTree(tree), IHS_SEMANTICS_OK);

  const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
  ASSERT_NE(snapshot, nullptr);
  EXPECT_EQ(ihs_semantics_snapshot_node_by_id(snapshot, 1)->enabled,
            IHS_SEMANTICS_TRISTATE_FALSE);
  EXPECT_EQ(ihs_semantics_snapshot_node_by_id(snapshot, 2)->enabled,
            IHS_SEMANTICS_TRISTATE_NONE);
  ihs_semantics_release_snapshot(snapshot);
}

TEST_F(SemanticsPublisherTest, ActionsAndIdentifierReachTheSnapshot) {
  AccessibilityTree tree;
  NodeBuilder root(0, {1}, "root");
  NodeBuilder slider(1, {}, "Volume");
  slider.node.identifier = "hvac.temp.driver";
  slider.node.flags__deprecated__ = kFlutterSemanticsFlagIsSlider;
  slider.node.actions = static_cast<FlutterSemanticsAction>(
      kFlutterSemanticsActionIncrease | kFlutterSemanticsActionDecrease);
  Apply(tree, {&root.node, &slider.node});

  ASSERT_EQ(PublishTree(tree), IHS_SEMANTICS_OK);
  const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
  ASSERT_NE(snapshot, nullptr);
  const IhsSemanticsNode* out = ihs_semantics_snapshot_node_by_id(snapshot, 1);
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out->identifier, "hvac.temp.driver");
  EXPECT_EQ(out->role, IHS_SEMANTICS_ROLE_SLIDER);
  EXPECT_EQ(out->actions,
            IHS_SEMANTICS_ACTION_INCREASE | IHS_SEMANTICS_ACTION_DECREASE);
  ihs_semantics_release_snapshot(snapshot);
}

// An obscured text field is reported as such so a consumer can decline to
// surface its value, without having to re-derive the role.
TEST_F(SemanticsPublisherTest, ObscuredIsReportedForPasswordFields) {
  AccessibilityTree tree;
  NodeBuilder root(0, {1}, "root");
  NodeBuilder password(1, {}, "Password");
  password.node.flags__deprecated__ = static_cast<FlutterSemanticsFlag>(
      kFlutterSemanticsFlagIsTextField | kFlutterSemanticsFlagIsObscured);
  Apply(tree, {&root.node, &password.node});

  ASSERT_EQ(PublishTree(tree), IHS_SEMANTICS_OK);
  const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
  ASSERT_NE(snapshot, nullptr);
  const IhsSemanticsNode* out = ihs_semantics_snapshot_node_by_id(snapshot, 1);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->role, IHS_SEMANTICS_ROLE_PASSWORD_INPUT);
  EXPECT_TRUE(out->obscured);
  ihs_semantics_release_snapshot(snapshot);
}

// Republishing an unchanged tree must produce the same node order, or a
// consumer diffing two snapshots sees churn that did not happen.
TEST_F(SemanticsPublisherTest, RepeatedPublishesAgreeOnOrder) {
  AccessibilityTree tree;
  NodeBuilder root(0, {1, 2}, "root");
  NodeBuilder a(1, {}, "a");
  NodeBuilder b(2, {}, "b");
  Apply(tree, {&root.node, &a.node, &b.node});

  std::vector<std::string> first;
  std::vector<std::string> second;
  for (int pass = 0; pass < 2; pass++) {
    ASSERT_EQ(PublishTree(tree), IHS_SEMANTICS_OK);
    const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
    ASSERT_NE(snapshot, nullptr);
    auto& into = pass == 0 ? first : second;
    for (size_t i = 0; i < ihs_semantics_snapshot_node_count(snapshot); i++) {
      into.emplace_back(ihs_semantics_snapshot_node_at(snapshot, i)->label);
    }
    ihs_semantics_release_snapshot(snapshot);
  }
  EXPECT_EQ(first, second);
}
