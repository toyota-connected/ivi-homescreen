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

#include <cstdint>
#include <vector>

#include "gtest/gtest.h"

#include "backend/drm_plane_formats.h"

namespace {

using drm::planes::DRMPlaneType;
using drm::planes::PlaneCapabilities;
using drm::planes::PlaneRegistry;

PlaneCapabilities Plane(uint32_t id, DRMPlaneType type, bool alpha) {
  PlaneCapabilities p;
  p.id = id;
  p.possible_crtcs = 1;  // CRTC index 0
  p.type = type;
  p.has_per_plane_alpha = alpha;
  return p;
}

bool AllHaveAlpha(std::vector<PlaneCapabilities> planes) {
  const PlaneRegistry reg = PlaneRegistry::from_capabilities(std::move(planes));
  return PlaneFormats(reg, 0, -1).AllHaveAlpha();
}

}  // namespace

TEST(DrmPlaneFormats, OverlaysWithAlphaShowATranslucentView) {
  EXPECT_TRUE(AllHaveAlpha({Plane(1, DRMPlaneType::PRIMARY, true),
                            Plane(2, DRMPlaneType::OVERLAY, true),
                            Plane(3, DRMPlaneType::OVERLAY, true)}));
}

TEST(DrmPlaneFormats, OneOverlayWithoutAlphaIsEnoughToSayNo) {
  // The allocator may put the view on either overlay, and the one without
  // alpha would show it opaque.
  EXPECT_FALSE(AllHaveAlpha({Plane(1, DRMPlaneType::PRIMARY, true),
                             Plane(2, DRMPlaneType::OVERLAY, true),
                             Plane(3, DRMPlaneType::OVERLAY, false)}));
}

TEST(DrmPlaneFormats, OnlyThePlanesAViewCanLandOnCount) {
  // With overlays, the primary carries the Flutter UI, and a cursor plane is
  // too small for a view: neither one's alpha matters.
  EXPECT_TRUE(AllHaveAlpha({Plane(1, DRMPlaneType::PRIMARY, false),
                            Plane(2, DRMPlaneType::OVERLAY, true),
                            Plane(3, DRMPlaneType::CURSOR, false)}));
}

TEST(DrmPlaneFormats, NoPlanesMeansNo) {
  EXPECT_FALSE(AllHaveAlpha({}));
  EXPECT_FALSE(PlaneFormats().AllHaveAlpha());
}
