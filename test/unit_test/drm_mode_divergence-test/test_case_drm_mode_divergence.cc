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

#include "backend/drm_kms_vulkan/vulkan_drm_backend.h"
#include "gtest/gtest.h"

namespace {

using Divergence = VulkanDrmBackend::ModeDivergence;

constexpr uint32_t k1080p60 = 16'666'667U;
constexpr uint32_t k1080p5994 = 16'683'350U;

TEST(DrmModeDivergence, SameModeIsNone) {
  EXPECT_EQ(Divergence::kNone, VulkanDrmBackend::ClassifyDivergence(
                                   1920, 1080, k1080p60, 1920, 1080, k1080p60));
}

// The case #762 is about: 59.94 and 60 differ by 16.6 us and the kernel's
// integer vrefresh reports both as 60, so the period is what has to be
// compared.
TEST(DrmModeDivergence, SameExtentDifferentTimingIsRefresh) {
  EXPECT_EQ(Divergence::kRefresh,
            VulkanDrmBackend::ClassifyDivergence(1920, 1080, k1080p60, 1920,
                                                 1080, k1080p5994));
}

TEST(DrmModeDivergence, DifferentExtentIsExtent) {
  EXPECT_EQ(Divergence::kExtent,
            VulkanDrmBackend::ClassifyDivergence(1920, 1080, k1080p60, 1280,
                                                 720, k1080p60));
}

// Extent wins: a mode that changed both is not a refresh change, because the
// swapchain has to be reallocated either way.
TEST(DrmModeDivergence, ExtentAndTimingTogetherIsExtent) {
  EXPECT_EQ(Divergence::kExtent,
            VulkanDrmBackend::ClassifyDivergence(1920, 1080, k1080p60, 1280,
                                                 720, k1080p5994));
}

TEST(DrmModeDivergence, EitherAxisAloneCounts) {
  EXPECT_EQ(Divergence::kExtent,
            VulkanDrmBackend::ClassifyDivergence(1920, 1080, k1080p60, 1920,
                                                 720, k1080p60));
  EXPECT_EQ(Divergence::kExtent,
            VulkanDrmBackend::ClassifyDivergence(1920, 1080, k1080p60, 1280,
                                                 1080, k1080p60));
}

}  // namespace
