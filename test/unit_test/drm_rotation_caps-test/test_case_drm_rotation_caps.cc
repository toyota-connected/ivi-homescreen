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

// What --drm-rotation needs from a display, decided from plane rotation bits,
// modifiers and the KMS driver (#838). The displays here are the ones the
// issue names: Sparrow Hawk (rcar-du) exposes no rotation property and scans
// out LINEAR only; amdgpu rotates but needs a tiled non-DCC buffer for 90/270;
// vkms rotates LINEAR by any angle.

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "backend/drm_kms_vulkan/rotation_caps.h"

extern "C" {
#include <drm_fourcc.h>
#include <drm_mode.h>
}

using drm_kms_vulkan::DescribeRotationBits;
using drm_kms_vulkan::LinearRotates;
using drm_kms_vulkan::RotatedScanoutModifiers;
using drm_kms_vulkan::RotationCompatible;
using drm_kms_vulkan::RotationSupported;
using drm_kms_vulkan::RotationToDrmFlag;

namespace {

constexpr uint64_t kAllAngles = DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_90 |
                                DRM_MODE_ROTATE_180 | DRM_MODE_ROTATE_270 |
                                DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y;
// A plane with a rotation property that implements only 0/180 and reflect,
// which a "has a rotation property" check would wrongly accept for 90.
constexpr uint64_t kHalfTurnOnly =
    DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_180 | DRM_MODE_REFLECT_X;

// An AMD GFX9 64K_S tiled modifier, without and with DCC.
const uint64_t kAmdTiled =
    AMD_FMT_MOD | AMD_FMT_MOD_SET(TILE_VERSION, AMD_FMT_MOD_TILE_VER_GFX9) |
    AMD_FMT_MOD_SET(TILE, AMD_FMT_MOD_TILE_GFX9_64K_S);
const uint64_t kAmdTiledDcc = kAmdTiled | AMD_FMT_MOD_SET(DCC, 1);

}  // namespace

TEST(DrmRotationCaps, DegreesMapToTheKernelsBits) {
  EXPECT_EQ(RotationToDrmFlag(0), DRM_MODE_ROTATE_0);
  EXPECT_EQ(RotationToDrmFlag(90), DRM_MODE_ROTATE_90);
  EXPECT_EQ(RotationToDrmFlag(180), DRM_MODE_ROTATE_180);
  EXPECT_EQ(RotationToDrmFlag(270), DRM_MODE_ROTATE_270);
}

// Sparrow Hawk: no plane exposes a rotation property, so there is no bit at
// all. Every turn has to be refused; only 0 stands.
TEST(DrmRotationCaps, NoRotationPropertyServesOnlyZero) {
  EXPECT_TRUE(RotationSupported(0, DRM_MODE_ROTATE_0));
  EXPECT_FALSE(RotationSupported(0, DRM_MODE_ROTATE_90));
  EXPECT_FALSE(RotationSupported(0, DRM_MODE_ROTATE_180));
  EXPECT_FALSE(RotationSupported(0, DRM_MODE_ROTATE_270));
}

TEST(DrmRotationCaps, APlaneMustOfferTheExactAngle) {
  EXPECT_TRUE(RotationSupported(kHalfTurnOnly, DRM_MODE_ROTATE_180));
  EXPECT_FALSE(RotationSupported(kHalfTurnOnly, DRM_MODE_ROTATE_90));
  EXPECT_FALSE(RotationSupported(kHalfTurnOnly, DRM_MODE_ROTATE_270));
  EXPECT_TRUE(RotationSupported(kAllAngles, DRM_MODE_ROTATE_270));
}

TEST(DrmRotationCaps, BitsAreDescribedAsTheKernelNamesThem) {
  EXPECT_EQ(DescribeRotationBits(0), "none");
  EXPECT_EQ(DescribeRotationBits(kHalfTurnOnly),
            "rotate-0,rotate-180,reflect-x");
  EXPECT_EQ(DescribeRotationBits(kAllAngles),
            "rotate-0,rotate-90,rotate-180,rotate-270,reflect-x,reflect-y");
}

// The rule used to be "90/270 never on LINEAR", which is amdgpu's. Applied to
// every driver it refused vkms and any LINEAR-only display that does rotate.
TEST(DrmRotationCaps, OnlyAmdgpuRefusesToRotateLinear) {
  EXPECT_FALSE(LinearRotates("amdgpu"));
  EXPECT_TRUE(LinearRotates("vkms"));
  EXPECT_TRUE(LinearRotates("rcar-du"));
  EXPECT_FALSE(RotationCompatible(DRM_FORMAT_MOD_LINEAR, "amdgpu"));
  EXPECT_TRUE(RotationCompatible(DRM_FORMAT_MOD_LINEAR, "vkms"));
}

TEST(DrmRotationCaps, AmdDccIsNeverRotated) {
  EXPECT_TRUE(RotationCompatible(kAmdTiled, "amdgpu"));
  EXPECT_FALSE(RotationCompatible(kAmdTiledDcc, "amdgpu"));
}

TEST(DrmRotationCaps, AmdgpuGetsItsTiledNonDccModifiers) {
  const std::vector<uint64_t> allowed{DRM_FORMAT_MOD_LINEAR, kAmdTiled,
                                      kAmdTiledDcc};
  EXPECT_EQ(RotatedScanoutModifiers(allowed, "amdgpu"),
            std::vector<uint64_t>{kAmdTiled});
}

// amdgpu offering LINEAR alone cannot serve 90/270 at all: empty, which the
// backend turns into a refusal at startup.
TEST(DrmRotationCaps, AmdgpuWithOnlyLinearCannotRotate) {
  EXPECT_TRUE(
      RotatedScanoutModifiers({DRM_FORMAT_MOD_LINEAR}, "amdgpu").empty());
  EXPECT_TRUE(
      RotatedScanoutModifiers({DRM_FORMAT_MOD_LINEAR, kAmdTiledDcc}, "amdgpu")
          .empty());
}

// vkms, and Sparrow Hawk's buffers: LINEAR is all there is, and the driver is
// left to rotate it (Sparrow Hawk is then refused on the plane bits instead).
TEST(DrmRotationCaps, ALinearOnlyDisplayRotatesLinear) {
  EXPECT_EQ(RotatedScanoutModifiers({DRM_FORMAT_MOD_LINEAR}, "vkms"),
            std::vector<uint64_t>{DRM_FORMAT_MOD_LINEAR});
}

// Tiled is still preferred where the driver has one, as before.
TEST(DrmRotationCaps, TiledIsPreferredOverLinear) {
  const uint64_t tiled = I915_FORMAT_MOD_Y_TILED;
  EXPECT_EQ(RotatedScanoutModifiers({DRM_FORMAT_MOD_LINEAR, tiled}, "i915"),
            std::vector<uint64_t>{tiled});
}

TEST(DrmRotationCaps, NothingNegotiatedServesNothing) {
  EXPECT_TRUE(RotatedScanoutModifiers({}, "vkms").empty());
}
