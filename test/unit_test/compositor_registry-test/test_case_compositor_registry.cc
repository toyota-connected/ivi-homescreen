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

// Exercises Phase 4's polymorphic compositor-surface dispatch. Builds only
// when BUILD_COMPOSITOR is defined; otherwise the virtual methods don't
// exist. No GL/Vulkan context is required — a RecordingBackend captures
// calls and a stub ICompositorSurface participates only as a reference.

#include <memory>
#include <utility>
#include <vector>

#include "backend/backend.h"
#include "gtest/gtest.h"
#include "view/compositor_surface_interface.h"

namespace {

class StubSurface : public ICompositorSurface {
 public:
  explicit StubSurface(FlutterPlatformViewIdentifier id) : id_(id) {}

  bool OnCreateBackingStore(const FlutterBackingStoreConfig*,
                            FlutterBackingStore*) override {
    return true;
  }
  bool OnCollectBackingStore(const FlutterBackingStore*) override {
    return true;
  }
  bool OnPresent(const FlutterLayer*) override { return true; }
  [[nodiscard]] FlutterPlatformViewIdentifier GetIdentifier() const override {
    return id_;
  }
  void OnResize(int32_t w, int32_t h) override {
    last_w_ = w;
    last_h_ = h;
    ++resize_count_;
  }

  int resize_count() const { return resize_count_; }
  int last_w() const { return last_w_; }
  int last_h() const { return last_h_; }

 private:
  FlutterPlatformViewIdentifier id_;
  int resize_count_{0};
  int32_t last_w_{0};
  int32_t last_h_{0};
};

class RecordingBackend : public Backend {
 public:
  // Pure virtuals — unused by these tests but must be satisfied.
  void Resize(size_t, Engine*, int32_t, int32_t) override {}
  void CreateSurface(size_t, struct wl_surface*, int32_t, int32_t) override {}
  bool TextureMakeCurrent() override { return true; }
  bool TextureClearCurrent() override { return true; }
  FlutterRendererConfig GetRenderConfig() override { return {}; }
  FlutterCompositor GetCompositorConfig() override { return {}; }

  void RegisterCompositorSurface(
      FlutterPlatformViewIdentifier id,
      std::shared_ptr<ICompositorSurface> surface) override {
    registered.emplace_back(id, std::move(surface));
  }
  void UnregisterCompositorSurface(FlutterPlatformViewIdentifier id) override {
    unregistered.push_back(id);
  }
  void ResizeCompositorSurface(FlutterPlatformViewIdentifier id,
                               int32_t width,
                               int32_t height) override {
    resized.emplace_back(id, width, height);
  }

  std::vector<std::pair<FlutterPlatformViewIdentifier,
                        std::shared_ptr<ICompositorSurface>>>
      registered;
  std::vector<FlutterPlatformViewIdentifier> unregistered;
  std::vector<std::tuple<FlutterPlatformViewIdentifier, int32_t, int32_t>>
      resized;
};

}  // namespace

TEST(CompositorRegistry, RegisterRoutesThroughBaseClassPointer) {
  auto backend = std::make_unique<RecordingBackend>();
  Backend* base = backend.get();

  auto s = std::make_shared<StubSurface>(7);
  base->RegisterCompositorSurface(7, s);

  ASSERT_EQ(backend->registered.size(), 1u);
  EXPECT_EQ(backend->registered[0].first, 7);
  EXPECT_EQ(backend->registered[0].second.get(), s.get());
}

TEST(CompositorRegistry, UnregisterRoutesThroughBaseClassPointer) {
  RecordingBackend backend;
  static_cast<Backend&>(backend).UnregisterCompositorSurface(12);
  ASSERT_EQ(backend.unregistered.size(), 1u);
  EXPECT_EQ(backend.unregistered[0], 12);
}

TEST(CompositorRegistry, ResizeRoutesThroughBaseClassPointer) {
  RecordingBackend backend;
  static_cast<Backend&>(backend).ResizeCompositorSurface(3, 640, 480);
  ASSERT_EQ(backend.resized.size(), 1u);
  EXPECT_EQ(std::get<0>(backend.resized[0]), 3);
  EXPECT_EQ(std::get<1>(backend.resized[0]), 640);
  EXPECT_EQ(std::get<2>(backend.resized[0]), 480);
}

// Default Backend methods (for backends that don't opt in to compositor
// mode) must be safe no-ops. This documents the contract for headless /
// DRM backends that haven't been updated.
TEST(CompositorRegistry, DefaultBackendImplementationsAreNoops) {
  class MinimalBackend : public Backend {
    void Resize(size_t, Engine*, int32_t, int32_t) override {}
    void CreateSurface(size_t, struct wl_surface*, int32_t, int32_t) override {}
    bool TextureMakeCurrent() override { return true; }
    bool TextureClearCurrent() override { return true; }
    FlutterRendererConfig GetRenderConfig() override { return {}; }
    FlutterCompositor GetCompositorConfig() override { return {}; }
  };

  MinimalBackend b;
  Backend& base = b;
  auto s = std::make_shared<StubSurface>(1);
  // Should not throw, not abort, not crash.
  base.RegisterCompositorSurface(1, s);
  base.UnregisterCompositorSurface(1);
  base.ResizeCompositorSurface(1, 10, 20);
  SUCCEED();
}

TEST(CompositorRegistry, StubSurfaceOnResizeIsDispatched) {
  // Verify ICompositorSurface::OnResize dispatch — the default virtual
  // method is a no-op, but overrides must fire.
  auto s = std::make_shared<StubSurface>(42);
  ICompositorSurface* base = s.get();
  base->OnResize(800, 600);
  EXPECT_EQ(s->resize_count(), 1);
  EXPECT_EQ(s->last_w(), 800);
  EXPECT_EQ(s->last_h(), 600);
}
