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

#include <vector>

#include "gtest/gtest.h"
#include "view/mutation_stack.h"

namespace {

FlutterPlatformView MakeView(
    std::vector<const FlutterPlatformViewMutation*>& list) {
  FlutterPlatformView v{};
  v.struct_size = sizeof(FlutterPlatformView);
  v.identifier = 1;
  v.mutations_count = list.size();
  v.mutations = list.data();
  return v;
}

FlutterPlatformViewMutation Opacity(double a) {
  FlutterPlatformViewMutation m{};
  m.type = kFlutterPlatformViewMutationTypeOpacity;
  m.opacity = a;
  return m;
}

FlutterPlatformViewMutation Translate(double tx, double ty) {
  FlutterPlatformViewMutation m{};
  m.type = kFlutterPlatformViewMutationTypeTransformation;
  m.transformation = {1, 0, tx, 0, 1, ty, 0, 0, 1};
  return m;
}

FlutterPlatformViewMutation Scale(double sx, double sy) {
  FlutterPlatformViewMutation m{};
  m.type = kFlutterPlatformViewMutationTypeTransformation;
  m.transformation = {sx, 0, 0, 0, sy, 0, 0, 0, 1};
  return m;
}

FlutterPlatformViewMutation ClipRect(double l, double t, double r, double b) {
  FlutterPlatformViewMutation m{};
  m.type = kFlutterPlatformViewMutationTypeClipRect;
  m.clip_rect = {l, t, r, b};
  return m;
}

FlutterPlatformViewMutation RoundedRect() {
  FlutterPlatformViewMutation m{};
  m.type = kFlutterPlatformViewMutationTypeClipRoundedRect;
  return m;
}

}  // namespace

TEST(MutationStack, EmptyOrNullProducesIdentity) {
  EXPECT_FALSE(MutationStack::Compose(nullptr).NeedsPluginComposite());
  FlutterPlatformView v{};
  v.struct_size = sizeof(FlutterPlatformView);
  v.mutations_count = 0;
  v.mutations = nullptr;
  const auto r = MutationStack::Compose(&v);
  EXPECT_DOUBLE_EQ(r.opacity, 1.0);
  EXPECT_DOUBLE_EQ(r.a, 1.0);
  EXPECT_DOUBLE_EQ(r.d, 1.0);
  EXPECT_FALSE(r.has_clip);
  EXPECT_TRUE(r.IsAxisAligned());
  EXPECT_FALSE(r.NeedsPluginComposite());
}

TEST(MutationStack, OpacityAccumulatesMultiplicatively) {
  auto m1 = Opacity(0.5);
  auto m2 = Opacity(0.5);
  std::vector<const FlutterPlatformViewMutation*> list{&m1, &m2};
  FlutterPlatformView v = MakeView(list);
  const auto r = MutationStack::Compose(&v);
  EXPECT_DOUBLE_EQ(r.opacity, 0.25);
  EXPECT_TRUE(r.NeedsPluginComposite());
}

TEST(MutationStack, ClipRectIntersects) {
  auto c1 = ClipRect(0, 0, 100, 100);
  auto c2 = ClipRect(50, 50, 200, 200);
  std::vector<const FlutterPlatformViewMutation*> list{&c1, &c2};
  FlutterPlatformView v = MakeView(list);
  const auto r = MutationStack::Compose(&v);
  EXPECT_TRUE(r.has_clip);
  EXPECT_DOUBLE_EQ(r.clip_rect.left, 50);
  EXPECT_DOUBLE_EQ(r.clip_rect.top, 50);
  EXPECT_DOUBLE_EQ(r.clip_rect.right, 100);
  EXPECT_DOUBLE_EQ(r.clip_rect.bottom, 100);
}

TEST(MutationStack, TranslateThenScaleComposes) {
  // Mutations are applied outer-to-inner. Composing translate(10, 20)
  // followed by scale(2, 3) means: first scale, then translate by 10,20 in
  // the post-scale frame — so a point (1, 1) maps to (10 + 2, 20 + 3).
  // The composed matrix should multiply translate * scale.
  auto t = Translate(10, 20);
  auto s = Scale(2, 3);
  std::vector<const FlutterPlatformViewMutation*> list{&t, &s};
  FlutterPlatformView v = MakeView(list);
  const auto r = MutationStack::Compose(&v);
  EXPECT_DOUBLE_EQ(r.a, 2.0);
  EXPECT_DOUBLE_EQ(r.d, 3.0);
  EXPECT_DOUBLE_EQ(r.tx, 10.0);
  EXPECT_DOUBLE_EQ(r.ty, 20.0);
  EXPECT_TRUE(r.IsAxisAligned());
  EXPECT_FALSE(r.NeedsPluginComposite());
}

TEST(MutationStack, RoundedClipFlagsPluginComposite) {
  auto r1 = RoundedRect();
  std::vector<const FlutterPlatformViewMutation*> list{&r1};
  FlutterPlatformView v = MakeView(list);
  const auto r = MutationStack::Compose(&v);
  EXPECT_TRUE(r.has_rounded_clip);
  EXPECT_TRUE(r.NeedsPluginComposite());
}

TEST(MutationStack, RotationIsNotAxisAligned) {
  FlutterPlatformViewMutation rot{};
  rot.type = kFlutterPlatformViewMutationTypeTransformation;
  // 90-degree rotation: (cos, -sin; sin, cos) = (0,-1;1,0)
  rot.transformation = {0, 1, 0, -1, 0, 0, 0, 0, 1};
  std::vector<const FlutterPlatformViewMutation*> list{&rot};
  FlutterPlatformView v = MakeView(list);
  const auto r = MutationStack::Compose(&v);
  EXPECT_FALSE(r.IsAxisAligned());
  EXPECT_TRUE(r.NeedsPluginComposite());
}

TEST(MutationStack, NullEntryIsSkipped) {
  std::vector<const FlutterPlatformViewMutation*> list{nullptr};
  FlutterPlatformView v = MakeView(list);
  const auto r = MutationStack::Compose(&v);
  EXPECT_FALSE(r.NeedsPluginComposite());
}
