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
#include <vector>

#include "gtest/gtest.h"
#include "view/present_layer_sequencer.h"

namespace {

// Opaque fake Wayland objects. Tests inject a Placer that records calls
// instead of talking to the Wayland client library.
struct FakeSurface {};
struct FakeSubsurface {};

struct PlaceCall {
  wl_subsurface* subsurface;
  wl_surface* sibling;
};

FlutterLayer MakeBackingStoreLayer() {
  FlutterLayer l{};
  l.struct_size = sizeof(FlutterLayer);
  l.type = kFlutterLayerContentTypeBackingStore;
  return l;
}

FlutterLayer MakePlatformViewLayer(FlutterPlatformView* pv) {
  FlutterLayer l{};
  l.struct_size = sizeof(FlutterLayer);
  l.type = kFlutterLayerContentTypePlatformView;
  l.platform_view = pv;
  return l;
}

}  // namespace

namespace {
PresentLayerSequencer::Placer NoopPlacer(std::vector<PlaceCall>* log) {
  return [log](wl_subsurface* subsurface, wl_surface* sibling) {
    if (log) {
      log->push_back({subsurface, sibling});
    }
  };
}
}  // namespace

TEST(PresentLayerSequencer, RegisterAndUnregister) {
  PresentLayerSequencer seq(NoopPlacer(nullptr));
  FakeSubsurface s;
  FakeSurface sur;
  seq.RegisterSubsurface(42, reinterpret_cast<wl_subsurface*>(&s),
                         reinterpret_cast<wl_surface*>(&sur));
  EXPECT_EQ(seq.RegisteredCount(), 1u);

  seq.UnregisterSubsurface(42);
  EXPECT_EQ(seq.RegisteredCount(), 0u);
}

TEST(PresentLayerSequencer, SingleBackingStoreLayerProducesEmptyOrder) {
  PresentLayerSequencer seq(NoopPlacer(nullptr));
  FlutterLayer bs = MakeBackingStoreLayer();
  const FlutterLayer* layers[] = {&bs};

  seq.Present(layers, 1, nullptr, nullptr);
  EXPECT_TRUE(seq.LastOrder().empty());
}

TEST(PresentLayerSequencer, UnregisteredPlatformViewInvokesMissingHandler) {
  PresentLayerSequencer seq(NoopPlacer(nullptr));
  FlutterPlatformView pv{};
  pv.struct_size = sizeof(FlutterPlatformView);
  pv.identifier = 7;
  FlutterLayer pvl = MakePlatformViewLayer(&pv);
  const FlutterLayer* layers[] = {&pvl};

  std::vector<FlutterPlatformViewIdentifier> missed;
  seq.Present(layers, 1, nullptr, nullptr,
              [&](FlutterPlatformViewIdentifier id) { missed.push_back(id); });

  ASSERT_EQ(missed.size(), 1u);
  EXPECT_EQ(missed[0], 7);
  EXPECT_TRUE(seq.LastOrder().empty());
}

TEST(PresentLayerSequencer, RegisteredPlatformViewRecordedInOrder) {
  PresentLayerSequencer seq(NoopPlacer(nullptr));
  FakeSubsurface sub;
  FakeSurface sur;
  seq.RegisterSubsurface(11, reinterpret_cast<wl_subsurface*>(&sub),
                         reinterpret_cast<wl_surface*>(&sur));

  FlutterPlatformView pv{};
  pv.struct_size = sizeof(FlutterPlatformView);
  pv.identifier = 11;
  FlutterLayer pvl = MakePlatformViewLayer(&pv);
  const FlutterLayer* layers[] = {&pvl};

  // Pass nullptr root_surface to avoid calling into wl_subsurface_place_above.
  seq.Present(layers, 1, nullptr, nullptr);
  ASSERT_EQ(seq.LastOrder().size(), 1u);
  EXPECT_EQ(seq.LastOrder()[0], 11);
}

TEST(PresentLayerSequencer, MultiLayerOrderReflectsInputOrder) {
  PresentLayerSequencer seq(NoopPlacer(nullptr));
  FakeSubsurface s1, s2;
  FakeSurface sur1, sur2;
  seq.RegisterSubsurface(1, reinterpret_cast<wl_subsurface*>(&s1),
                         reinterpret_cast<wl_surface*>(&sur1));
  seq.RegisterSubsurface(2, reinterpret_cast<wl_subsurface*>(&s2),
                         reinterpret_cast<wl_surface*>(&sur2));

  FlutterPlatformView pv1{}, pv2{};
  pv1.struct_size = sizeof(FlutterPlatformView);
  pv1.identifier = 1;
  pv2.struct_size = sizeof(FlutterPlatformView);
  pv2.identifier = 2;
  FlutterLayer bs = MakeBackingStoreLayer();
  FlutterLayer l1 = MakePlatformViewLayer(&pv1);
  FlutterLayer l2 = MakePlatformViewLayer(&pv2);
  const FlutterLayer* layers[] = {&bs, &l1, &l2};

  seq.Present(layers, 3, nullptr, nullptr);
  ASSERT_EQ(seq.LastOrder().size(), 2u);
  EXPECT_EQ(seq.LastOrder()[0], 1);
  EXPECT_EQ(seq.LastOrder()[1], 2);
}

TEST(PresentLayerSequencer, PlacerCalledWithSiblingChain) {
  std::vector<PlaceCall> calls;
  PresentLayerSequencer seq(NoopPlacer(&calls));
  FakeSubsurface s1, s2;
  FakeSurface sur1, sur2, root;
  seq.RegisterSubsurface(1, reinterpret_cast<wl_subsurface*>(&s1),
                         reinterpret_cast<wl_surface*>(&sur1));
  seq.RegisterSubsurface(2, reinterpret_cast<wl_subsurface*>(&s2),
                         reinterpret_cast<wl_surface*>(&sur2));

  FlutterPlatformView pv1{}, pv2{};
  pv1.struct_size = sizeof(FlutterPlatformView);
  pv1.identifier = 1;
  pv2.struct_size = sizeof(FlutterPlatformView);
  pv2.identifier = 2;
  FlutterLayer l1 = MakePlatformViewLayer(&pv1);
  FlutterLayer l2 = MakePlatformViewLayer(&pv2);
  const FlutterLayer* layers[] = {&l1, &l2};

  seq.Present(layers, 2, nullptr, reinterpret_cast<wl_surface*>(&root));
  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[0].subsurface, reinterpret_cast<wl_subsurface*>(&s1));
  EXPECT_EQ(calls[0].sibling, reinterpret_cast<wl_surface*>(&root));
  EXPECT_EQ(calls[1].subsurface, reinterpret_cast<wl_subsurface*>(&s2));
  EXPECT_EQ(calls[1].sibling, reinterpret_cast<wl_surface*>(&sur1));
}
