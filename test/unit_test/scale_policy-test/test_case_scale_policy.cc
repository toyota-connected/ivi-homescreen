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

// Conformance test for the fractional-scale rounding policy. ivi-homescreen's
// ScalePolicy is a backend-agnostic, dependency-free reimplementation of the
// buffer-at-physical rounding that wayland-cxx-scanner defines normatively
// (<wl/scale_policy.hpp>). Two normative implementations of the same rounding
// is one pixel of divergence waiting to happen at some fractional scale, so the
// contract is a SHARED conformance table: wcs publishes it as
// <wl/scale_policy_vectors.hpp> and every implementation must reproduce it.
//
// Rather than hand-copy the numbers (which can silently drift from the
// canonical table), this test includes that header directly from the vendored
// submodule and drives ScalePolicy against `kToBufferVectors` /
// `kCanvasScaleVectors`. The header is self-contained (only <cstdint>) and
// pulls in no Wayland, so a build that links none can still assert against it.
// A submodule bump that changes a vector then forces ScalePolicy to match or
// this test fails — which is exactly the lock-step the shared table exists for.

#include <cmath>

#include <gtest/gtest.h>

#include <wl/scale_policy_vectors.hpp>

#include "display/scale_policy.h"

using ivi::ScalePolicy;
namespace conf = wl::scale_conformance;

namespace {

// --- The canonical shared table (single source of truth) -------------------

// Every buffer-size vector wcs publishes must round identically here.
TEST(ScalePolicy, MatchesCanonicalToBufferVectors) {
  ASSERT_GT(conf::kToBufferVectorCount, 0U)
      << "canonical vector table is empty";
  for (std::size_t i = 0; i < conf::kToBufferVectorCount; ++i) {
    const conf::ToBufferVector& v = conf::kToBufferVectors[i];
    const auto b = ScalePolicy::ToBuffer(v.logical_w, v.logical_h, v.scale_120);
    EXPECT_EQ(b.width, v.expect_w)
        << "vector[" << i << "] logical_w=" << v.logical_w
        << " scale_120=" << v.scale_120;
    EXPECT_EQ(b.height, v.expect_h)
        << "vector[" << i << "] logical_h=" << v.logical_h
        << " scale_120=" << v.scale_120;
  }
}

// Every canvas-scale vector wcs publishes must match.
TEST(ScalePolicy, MatchesCanonicalCanvasScaleVectors) {
  ASSERT_GT(conf::kCanvasScaleVectorCount, 0U)
      << "canonical canvas-scale table is empty";
  for (std::size_t i = 0; i < conf::kCanvasScaleVectorCount; ++i) {
    const conf::CanvasScaleVector& v = conf::kCanvasScaleVectors[i];
    EXPECT_DOUBLE_EQ(ScalePolicy::CanvasScale(v.scale_120), v.expect)
        << "vector[" << i << "] scale_120=" << v.scale_120;
  }
}

// --- ivi-specific properties beyond the shared table -----------------------

// The earlier path computed std::lround(dim * (scale_120 / 120.0)). At exact
// halves the double product lands just under .5 and truncates down, one pixel
// short of the spec's round-half-up. This guards that ScalePolicy takes the
// integer (spec) side AND that the old form genuinely differed there, so the
// half-up cases in the shared table are protecting a real divergence rather
// than a tautology.
TEST(ScalePolicy, MatchesSpecWhereFloatingPointRoundedDown) {
  struct V {
    int32_t dim, scale_120, expect;
  };
  constexpr V kCases[] = {
      {100, 123, 103},   // 102.5  (double form gave 102)
      {60, 123, 62},     // 61.5   (double form gave 61)
      {990, 122, 1007},  // 1006.5 (double form gave 1006)
  };
  for (const auto& c : kCases) {
    EXPECT_EQ(ScalePolicy::ScaledDim(c.dim, c.scale_120), c.expect)
        << "dim=" << c.dim << " scale_120=" << c.scale_120;
    const auto old_form = static_cast<int32_t>(
        std::lround(c.dim * (static_cast<double>(c.scale_120) / 120.0)));
    EXPECT_EQ(old_form, c.expect - 1)
        << "old lround form expected to be one short at dim=" << c.dim;
  }
}

// Integer wl_output.scale N is carried as N*120; the buffer is an exact
// multiple and the canvas scale is exactly N, across the integer range.
TEST(ScalePolicy, IntegerOutputScaleIsExactMultiple) {
  for (int32_t n = 1; n <= 4; ++n) {
    EXPECT_EQ(ScalePolicy::ScaledDim(1000, n * 120), 1000 * n);
    EXPECT_DOUBLE_EQ(ScalePolicy::CanvasScale(n * 120), static_cast<double>(n));
  }
}

TEST(ScalePolicy, UnityConstant) {
  EXPECT_EQ(ScalePolicy::kUnityScale120, 120);
  EXPECT_EQ(ScalePolicy::kUnityScale120, conf::kToBufferVectors[0].scale_120);
}

}  // namespace
