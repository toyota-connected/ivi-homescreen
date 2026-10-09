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

// The FlutterEngineDisplay both notify paths build (#760): the rate precedence
// #732 established, and the scale conversion #150 established, in the one
// place that FlutterView::Initialize and FlutterView::UpdateDisplayMetadata
// now share. Pure arithmetic -- no engine, no backend, no display.

#include <gtest/gtest.h>

#include "backend/backend.h"
#include "view/display_metadata.h"

using homescreen::MakeDisplayMetadata;
using homescreen::ViewExtent;

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

// A Backend that paces to a mode, as the DRM and software backends do.
class ScanoutBackend final : public SilentBackend {
 public:
  [[nodiscard]] uint32_t RefreshPeriodNs() const override { return period_ns; }
  uint32_t period_ns{0};
};

}  // namespace

TEST(DisplayMetadata, TheBackendsRateWinsOverTheDisplaysFigure) {
  // A 56Hz mode on a display whose own figure is the constructed 60: the
  // backend owns the connector, so its answer is the one reported (#732).
  const FlutterEngineDisplay d =
      MakeDisplayMetadata(56.0, 60.0, ViewExtent{800, 600, 1.0}, 1.0);
  EXPECT_DOUBLE_EQ(d.refresh_rate, 56.0);
}

TEST(DisplayMetadata, TheDisplaysFigureIsUsedWhenTheBackendDrivesNoScanout) {
  // Wayland: the backend reports 0 and the wl_output refresh is the right
  // per-output answer.
  const FlutterEngineDisplay d =
      MakeDisplayMetadata(0.0, 59.94, ViewExtent{1920, 1080, 1.0}, 1.0);
  EXPECT_DOUBLE_EQ(d.refresh_rate, 59.94);
}

TEST(DisplayMetadata, NothingKnowsTheRate) {
  // Both sources silent -- a headless sink on a display that reports 0. The
  // zero is passed through rather than papered over with a 60 nobody measured.
  const FlutterEngineDisplay d =
      MakeDisplayMetadata(0.0, 0.0, ViewExtent{640, 480, 1.0}, 1.0);
  EXPECT_DOUBLE_EQ(d.refresh_rate, 0.0);
}

TEST(DisplayMetadata, AModeExtentIsReportedAsIs) {
  // DRM and software: already physical, scale 1.
  const FlutterEngineDisplay d =
      MakeDisplayMetadata(60.0, 60.0, ViewExtent{1920, 1080, 1.0}, 1.0);
  EXPECT_EQ(d.width, 1920u);
  EXPECT_EQ(d.height, 1080u);
  EXPECT_DOUBLE_EQ(d.device_pixel_ratio, 1.0);
}

TEST(DisplayMetadata, AScaledWaylandExtentIsReportedInPhysicalPixels) {
  // Logical dims times the surface scale, and the ratio scales with it (#150).
  const FlutterEngineDisplay d =
      MakeDisplayMetadata(0.0, 60.0, ViewExtent{1280, 720, 2.0}, 1.0);
  EXPECT_EQ(d.width, 2560u);
  EXPECT_EQ(d.height, 1440u);
  EXPECT_DOUBLE_EQ(d.device_pixel_ratio, 2.0);
}

TEST(DisplayMetadata, AFractionalScaleRoundsRatherThanTruncates) {
  // 1.25 on an extent whose scaled size lands past the halfway point:
  // 803 * 1.25 = 1003.75 and 603 * 1.25 = 753.75, so truncation loses a pixel
  // on each axis. Checked against a truncating mutant, which fails here.
  const FlutterEngineDisplay d =
      MakeDisplayMetadata(0.0, 60.0, ViewExtent{803, 603, 1.25}, 1.0);
  EXPECT_EQ(d.width, 1004u);
  EXPECT_EQ(d.height, 754u);
}

TEST(DisplayMetadata, TheConfiguredPixelRatioCompoundsWithTheScale) {
  const FlutterEngineDisplay d =
      MakeDisplayMetadata(0.0, 60.0, ViewExtent{1280, 720, 1.5}, 2.0);
  EXPECT_DOUBLE_EQ(d.device_pixel_ratio, 3.0);
}

TEST(DisplayMetadata, TheStructIsTaggedForTheEngine) {
  const FlutterEngineDisplay d =
      MakeDisplayMetadata(60.0, 60.0, ViewExtent{640, 480, 1.0}, 1.0);
  EXPECT_EQ(d.struct_size, sizeof(FlutterEngineDisplay));
  EXPECT_EQ(d.display_id, 1u);
  EXPECT_TRUE(d.single_display);
}

TEST(DisplayMetadata, ABackendsRateComesFromThePeriodItPacesTo) {
  // The conversion FlutterView feeds MakeDisplayMetadata. Not virtual, so a
  // backend cannot report a rate that disagrees with its own period (#732).
  ScanoutBackend backend;
  EXPECT_DOUBLE_EQ(backend.RefreshRateHz(), 0.0) << "no mode yet";

  backend.period_ns = 16'666'667;
  EXPECT_NEAR(backend.RefreshRateHz(), 60.0, 0.001);

  backend.period_ns = 17'857'142;
  EXPECT_NEAR(backend.RefreshRateHz(), 56.0, 0.001);

  const SilentBackend silent;
  EXPECT_DOUBLE_EQ(static_cast<const Backend&>(silent).RefreshRateHz(), 0.0);
}
