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

// Conformance vectors for the fractional-scale rounding policy. These mirror
// the canonical shared table published by wayland-cxx-scanner
// (include/wl/scale_policy_vectors.hpp), which our ScalePolicy is a
// backend-agnostic, dependency-free reimplementation of — asserting against
// the same numbers is what keeps the two from diverging by a pixel. This
// project cannot link the wcs header (its DRM/KMS and software backends pull in
// no Wayland), so the vectors are reproduced here; any change to the canonical
// table must be reflected in both.

#include <cmath>

#include <gtest/gtest.h>

#include "display/scale_policy.h"

using ivi::ScalePolicy;

namespace {

// --- Shared vector table (identical to wcs ScalePolicy) --------------------

TEST(ScalePolicy, UnityScaleIsIdentity) {
  const auto b = ScalePolicy::ToBuffer(480, 320, 120);
  EXPECT_EQ(b.width, 480);
  EXPECT_EQ(b.height, 320);
}

TEST(ScalePolicy, IntegralFractions) {
  const auto at125 = ScalePolicy::ToBuffer(480, 320, 150);  // 1.25
  EXPECT_EQ(at125.width, 600);
  EXPECT_EQ(at125.height, 400);
  const auto at150 = ScalePolicy::ToBuffer(480, 320, 180);  // 1.5
  EXPECT_EQ(at150.width, 720);
  EXPECT_EQ(at150.height, 480);
  const auto at2x = ScalePolicy::ToBuffer(480, 320, 240);  // 2.0
  EXPECT_EQ(at2x.width, 960);
  EXPECT_EQ(at2x.height, 640);
}

TEST(ScalePolicy, RoundsToNearestHalfUp) {
  EXPECT_EQ(ScalePolicy::ScaledDim(100, 123), 103);  // 102.5  -> 103
  EXPECT_EQ(ScalePolicy::ScaledDim(100, 127), 106);  // 105.83 -> 106
  EXPECT_EQ(ScalePolicy::ScaledDim(100, 122), 102);  // 101.67 -> 102
  EXPECT_EQ(ScalePolicy::ScaledDim(100, 126), 105);  // 105.0  -> 105 (exact)
}

TEST(ScalePolicy, NonPositiveScaleTreatedAsUnity) {
  EXPECT_EQ(ScalePolicy::ToBuffer(480, 320, 0).width, 480);
  EXPECT_EQ(ScalePolicy::ToBuffer(480, 320, 0).height, 320);
  EXPECT_EQ(ScalePolicy::ToBuffer(480, 320, -10).width, 480);
}

TEST(ScalePolicy, ZeroLogicalSizeStaysZero) {
  const auto b = ScalePolicy::ToBuffer(0, 0, 180);
  EXPECT_EQ(b.width, 0);
  EXPECT_EQ(b.height, 0);
}

TEST(ScalePolicy, LargeSizesDoNotOverflow) {
  const auto b = ScalePolicy::ToBuffer(4096, 4096, 360);  // 3.0
  EXPECT_EQ(b.width, 12288);
  EXPECT_EQ(b.height, 12288);
}

TEST(ScalePolicy, CanvasScaleMatchesFraction) {
  EXPECT_DOUBLE_EQ(ScalePolicy::CanvasScale(120), 1.0);
  EXPECT_DOUBLE_EQ(ScalePolicy::CanvasScale(180), 1.5);
  EXPECT_DOUBLE_EQ(ScalePolicy::CanvasScale(240), 2.0);
}

TEST(ScalePolicy, CanvasScaleNonPositiveIsUnity) {
  EXPECT_DOUBLE_EQ(ScalePolicy::CanvasScale(0), 1.0);
  EXPECT_DOUBLE_EQ(ScalePolicy::CanvasScale(-5), 1.0);
}

TEST(ScalePolicy, UnityConstant) {
  EXPECT_EQ(ScalePolicy::kUnityScale120, 120);
}

// --- Regression: exact integer rounding vs the old lround(dim*scale/120.0) --

// The earlier path computed std::lround(dim * (scale_120 / 120.0)). At exact
// halves the double product lands just under .5 and truncates down, one pixel
// short of the spec's round-half-up. These are the vectors where the two
// disagreed; the policy must match the integer (spec) side.
TEST(ScalePolicy, MatchesSpecWhereFloatingPointRoundedDown) {
  struct V {
    int32_t dim, scale_120, expect;
  };
  // dim*scale/120 lands on an exact .5 the double form rounds down.
  constexpr V kCases[] = {
      {100, 123, 103},   // 102.5  (double form gave 102)
      {60, 123, 62},     // 61.5   (double form gave 61)
      {990, 122, 1007},  // 1006.5 (double form gave 1006)
  };
  for (const auto& c : kCases) {
    EXPECT_EQ(ScalePolicy::ScaledDim(c.dim, c.scale_120), c.expect)
        << "dim=" << c.dim << " scale_120=" << c.scale_120;
    // And demonstrate the old form actually differed, so this test is guarding
    // a real divergence rather than a tautology.
    const auto old_form = static_cast<int32_t>(
        std::lround(c.dim * (static_cast<double>(c.scale_120) / 120.0)));
    EXPECT_EQ(old_form, c.expect - 1)
        << "old lround form expected to be one short at dim=" << c.dim;
  }
}

// Integer wl_output.scale N is carried as N*120; buffer is an exact multiple.
TEST(ScalePolicy, IntegerOutputScaleIsExactMultiple) {
  for (int32_t n = 1; n <= 4; ++n) {
    EXPECT_EQ(ScalePolicy::ScaledDim(1000, n * 120), 1000 * n);
    EXPECT_DOUBLE_EQ(ScalePolicy::CanvasScale(n * 120), static_cast<double>(n));
  }
}

}  // namespace
