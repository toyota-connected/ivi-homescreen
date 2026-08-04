/*
 * Copyright 2020-2026 Toyota Connected North America
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

// Backend::BoundOutputName() -- the output a backend is actually presenting
// on, as opposed to the one its config asked for. FlutterView::BoundOutput()
// prefers it over anything the resolver produced, so a backend that picks a
// connector on its own still reports a binding a hotplug listener can diff.
//
// Only the base-class contract is exercised here: whether DrmBackend records
// the connector it programmed needs a real card and is verified on hardware.

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "backend/backend.h"

namespace {

// A Backend that reports nothing, as every backend does until it overrides.
class SilentBackend : public Backend {
 public:
  // Pure virtuals -- unused here but must be satisfied.
  void Resize(size_t, Engine*, int32_t, int32_t) override {}
  void CreateSurface(size_t, struct wl_surface*, int32_t, int32_t) override {}
  bool TextureMakeCurrent() override { return true; }
  bool TextureClearCurrent() override { return true; }
  FlutterRendererConfig GetRenderConfig() override { return {}; }
  FlutterCompositor GetCompositorConfig() override { return {}; }
};

// A Backend that programmed an output and says which.
class BoundBackend : public SilentBackend {
 public:
  [[nodiscard]] std::optional<std::string> BoundOutputName() const override {
    return name;
  }
  std::optional<std::string> name{"DSI-1"};
};

}  // namespace

TEST(BackendOutputBinding, DefaultReportsNoBinding) {
  const SilentBackend backend;
  EXPECT_FALSE(static_cast<const Backend&>(backend).BoundOutputName());
}

TEST(BackendOutputBinding, OverrideIsReachedThroughTheBaseClass) {
  const BoundBackend backend;
  // Through a base reference: FlutterView holds a shared_ptr<Backend> and
  // never knows which backend it has, so a non-virtual call would silently
  // report "no binding" for every DRM view.
  const auto name = static_cast<const Backend&>(backend).BoundOutputName();
  ASSERT_TRUE(name.has_value());
  EXPECT_EQ(*name, "DSI-1");
}

TEST(BackendOutputBinding, ABackendMayReportNothingBeforeItPicks) {
  BoundBackend backend;
  // DrmBackend leaves this empty until a connector is picked; a caller must
  // treat "not yet" and "never" alike rather than assuming an override
  // always answers.
  backend.name.reset();
  EXPECT_FALSE(static_cast<const Backend&>(backend).BoundOutputName());
}
