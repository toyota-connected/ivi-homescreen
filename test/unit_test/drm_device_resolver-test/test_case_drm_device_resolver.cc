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

// Unit tests for homescreen::ResolveDrmDevice.
//
// Only the explicit-device early-return branch (branch 1 of 5) is tested here
// because it does not touch /dev/dri or open any DRM file descriptor.
//
// TODO(phase3): The remaining branches require real or faked DRM card nodes:
//   - Branch 2: ListCardNodes() returns empty   → nullopt (no /dev/dri/card*)
//   - Branch 3: single card present             → returned verbatim
//   - Branch 4: multiple cards, one connected   → first connected card
//   - Branch 5: multiple cards, none connected  → nullopt + diagnostics
//
// To test these without real hardware, ResolveDrmDevice should be refactored
// to accept an injectable dri_root path (default "/dev/dri"), allowing tests
// to pass a tmpdir populated with synthetic card* symlinks.  See the Phase 3
// plan in UNIT_TEST_PLAN.md (drm_device_resolver — injectable DRI path) for
// the seam-injection design and the matching production-code change.

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "display/drm_device_resolver.h"

using homescreen::ResolveDrmDevice;

// ---- Explicit device path -------------------------------------------------

TEST(DrmDeviceResolver, ExplicitDevice_ReturnsVerbatim) {
  // Branch 1: an explicit non-empty path wins immediately — no probing.
  const std::string kPath = "my/custom/card";
  const auto result = ResolveDrmDevice(std::optional<std::string>{kPath});
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, kPath);
}

TEST(DrmDeviceResolver, ExplicitDevice_AbsolutePath_ReturnsVerbatim) {
  const std::string kPath = "/dev/dri/card1";
  const auto result = ResolveDrmDevice(std::optional<std::string>{kPath});
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, kPath);
}

TEST(DrmDeviceResolver, ExplicitDevice_UnusualPath_ReturnsVerbatim) {
  // Verifies the function does not validate or normalise the path.
  const std::string kPath = "/tmp/fake-dri/cardX";
  const auto result = ResolveDrmDevice(std::optional<std::string>{kPath});
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, kPath);
}

// ---- Nullopt and empty explicit (fall-through) ----------------------------

TEST(DrmDeviceResolver, NulloptExplicit_DoesNotCrash) {
  // Falls through to ListCardNodes().  The result depends on the host
  // environment (CI may have no /dev/dri), so only verify no crash/exception.
  EXPECT_NO_FATAL_FAILURE(ResolveDrmDevice(std::nullopt));
}

TEST(DrmDeviceResolver, EmptyExplicit_DoesNotCrash) {
  // An explicitly empty string also falls through to ListCardNodes().
  EXPECT_NO_FATAL_FAILURE(ResolveDrmDevice(std::optional<std::string>{""}));
}
