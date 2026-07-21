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

// Exercises PlatformViewRegistry: the legacy callback-table lifecycle, the
// runtime factory path, and registry-owned instance destruction on dispose.
// No engine state is needed — a null state disables only the compositor-surface
// safety nets, which are guarded.

#include <memory>

#include "gtest/gtest.h"
#include "platform/homescreen/platform_views/platform_view.h"
#include "platform/homescreen/platform_views/platform_view_listener.h"
#include "platform/homescreen/platform_views/platform_view_registry.h"

namespace {

// Captured through the listener context so callbacks are observable.
struct Probe {
  int resize = 0;
  int dispose = 0;
  double last_w = 0;
  double last_h = 0;
};

void ProbeResize(double w, double h, void* data) {
  auto* p = static_cast<Probe*>(data);
  ++p->resize;
  p->last_w = w;
  p->last_h = h;
}

void ProbeDispose(bool /* hybrid */, void* data) {
  ++static_cast<Probe*>(data)->dispose;
}

constexpr platform_view_listener kListener = {
    ProbeResize,   // resize
    nullptr,       // set_direction
    nullptr,       // set_offset
    nullptr,       // on_touch
    ProbeDispose,  // dispose
    nullptr,       // accept_gesture
    nullptr,       // reject_gesture
};

// A factory-created instance that flips a flag when destroyed, so the test can
// prove the registry owns and releases it on dispose.
class FakeView : public PlatformView {
 public:
  FakeView(const PlatformViewRegistry::CreateRequest& r, bool* destroyed)
      : PlatformView(r.id,
                     r.view_type,
                     r.direction,
                     r.left,
                     r.top,
                     r.width,
                     r.height),
        destroyed_(destroyed) {}
  ~FakeView() override {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

 private:
  bool* destroyed_;
};

// P1.1: the registry dispatches the callback table and drops it on dispose.
TEST(PlatformViewRegistry, LegacyListenerLifecycle) {
  PlatformViewRegistry registry(nullptr);
  Probe probe;

  registry.RegisterListener(3, &kListener, &probe);
  registry.Resize(3, 100.0, 200.0);
  EXPECT_EQ(probe.resize, 1);
  EXPECT_DOUBLE_EQ(probe.last_w, 100.0);
  EXPECT_DOUBLE_EQ(probe.last_h, 200.0);

  EXPECT_TRUE(registry.Dispose(3, false));
  EXPECT_EQ(probe.dispose, 1);

  // After dispose the instance is gone: further dispatch is a no-op.
  registry.Resize(3, 5.0, 5.0);
  EXPECT_EQ(probe.resize, 1);
  EXPECT_FALSE(registry.Dispose(3, false));
}

// A repeated registration for a live id no longer toggles the entry off; the
// new view wins. The old owned instance is dropped and the callback table is
// replaced in place, so the id stays live and dispatch reaches the listener.
// (The old toggle erased the entry, which CreateViaFactory then resurrected
// with a null listener -> a later Resize dereferenced it and crashed.)
TEST(PlatformViewRegistry, RepeatedRegistrationReplacesInPlace) {
  PlatformViewRegistry registry(nullptr);
  Probe probe;

  registry.RegisterListener(1, &kListener, &probe);
  registry.RegisterListener(1, &kListener,
                            &probe);  // new view wins, still live
  registry.Resize(1, 10.0, 10.0);
  EXPECT_EQ(probe.resize, 1);
}

// P1.2: no factory for a type -> caller falls back (CreateViaFactory is false).
TEST(PlatformViewRegistry, CreateFallsBackWithoutFactory) {
  PlatformViewRegistry registry(nullptr);
  PlatformViewRegistry::CreateRequest req;
  req.id = 9;
  req.view_type = "unregistered";
  EXPECT_FALSE(registry.HasFactory("unregistered"));
  EXPECT_FALSE(registry.CreateViaFactory(req));
}

// P1.2 + P1.3: a factory creates an instance the registry owns, and dispose
// destroys it.
TEST(PlatformViewRegistry, FactoryOwnedInstanceDestroyedOnDispose) {
  PlatformViewRegistry registry(nullptr);
  Probe probe;
  bool destroyed = false;

  registry.RegisterFactory("fake",
                           [&](PlatformViewRegistry& reg,
                               const PlatformViewRegistry::CreateRequest& r) {
                             reg.RegisterListener(r.id, &kListener, &probe);
                             return std::make_unique<FakeView>(r, &destroyed);
                           });
  EXPECT_TRUE(registry.HasFactory("fake"));

  PlatformViewRegistry::CreateRequest req;
  req.id = 7;
  req.view_type = "fake";
  req.width = 320.0;
  req.height = 240.0;

  EXPECT_TRUE(registry.CreateViaFactory(req));
  EXPECT_FALSE(destroyed);

  // The registered table still dispatches to the live instance.
  registry.Resize(7, 42.0, 24.0);
  EXPECT_EQ(probe.resize, 1);

  EXPECT_TRUE(registry.Dispose(7, false));
  EXPECT_EQ(probe.dispose, 1);
  EXPECT_TRUE(destroyed);  // registry-owned instance released on dispose
}

}  // namespace
