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

#include <cmath>
#include <cstdint>
#include <utility>

#include "gtest/gtest.h"

#include "display_rotation.h"

namespace {

constexpr int kAngles[] = {0, 90, 180, 270};

// A non-square render extent on a panel it fills once turned, so a swapped
// width and height shows.
DisplayRotation Filling(const int degrees) {
  constexpr int32_t kW = 7;
  constexpr int32_t kH = 4;
  const bool swap = degrees == 90 || degrees == 270;
  return {degrees, kW, kH, swap ? kH : kW, swap ? kW : kH};
}

// Panel pixel holding render pixel (x, y): its center through ToPanel.
std::pair<int, int> PanelPixel(const DisplayRotation& r, int x, int y) {
  double px = 0;
  double py = 0;
  r.ToPanel(x + 0.5, y + 0.5, &px, &py);
  return {static_cast<int>(std::floor(px)), static_cast<int>(std::floor(py))};
}

// Where the DRM seat puts the HW cursor for render pixel (x, y): its
// FlushCursorMotion, which drm-kms-vulkan's plane rotation was checked
// against. The GPU path has to agree with it or the cursor walks off the
// picture.
std::pair<int, int> SeatCursor(const DisplayRotation& r, int x, int y) {
  switch (r.degrees) {
    case 90:
      return {y, r.render_w - 1 - x};
    case 180:
      return {r.render_w - 1 - x, r.render_h - 1 - y};
    case 270:
      return {r.render_h - 1 - y, x};
    default:
      return {x, y};
  }
}

}  // namespace

// The direction of the turn is DRM_MODE_ROTATE_*'s: counter-clockwise. At 90
// the render picture's top-right corner is the panel's top-left.
TEST(DisplayRotation, NinetyTurnsCounterClockwise) {
  const DisplayRotation r = Filling(90);
  EXPECT_EQ(PanelPixel(r, r.render_w - 1, 0), std::make_pair(0, 0));
  EXPECT_EQ(PanelPixel(r, 0, 0), std::make_pair(0, r.panel_h - 1));
  EXPECT_EQ(PanelPixel(r, 0, r.render_h - 1),
            std::make_pair(r.panel_w - 1, r.panel_h - 1));
}

TEST(DisplayRotation, EveryPixelLandsWhereTheSeatPutsTheCursor) {
  for (const int deg : kAngles) {
    SCOPED_TRACE(deg);
    const DisplayRotation r = Filling(deg);
    for (int y = 0; y < r.render_h; ++y) {
      for (int x = 0; x < r.render_w; ++x) {
        EXPECT_EQ(PanelPixel(r, x, y), SeatCursor(r, x, y));
      }
    }
  }
}

TEST(DisplayRotation, ARectStaysWholeAndOnThePanel) {
  for (const int deg : kAngles) {
    SCOPED_TRACE(deg);
    const DisplayRotation r = Filling(deg);
    const RectI whole = r.ToPanel(RectI{0, 0, r.render_w, r.render_h});
    EXPECT_EQ(whole.x, 0);
    EXPECT_EQ(whole.y, 0);
    EXPECT_EQ(whole.w, r.panel_w);
    EXPECT_EQ(whole.h, r.panel_h);

    // A sub-rect covers exactly the panel pixels its render pixels land on.
    const RectI sub{1, 1, 3, 2};
    const RectI p = r.ToPanel(sub);
    for (int y = sub.y; y < sub.y + sub.h; ++y) {
      for (int x = sub.x; x < sub.x + sub.w; ++x) {
        const auto [px, py] = PanelPixel(r, x, y);
        EXPECT_GE(px, p.x);
        EXPECT_LT(px, p.x + p.w);
        EXPECT_GE(py, p.y);
        EXPECT_LT(py, p.y + p.h);
      }
    }
    EXPECT_EQ(p.w * p.h, sub.w * sub.h);
  }
}

// A picture smaller than the panel, as a configured width and height leave
// it, is centered once turned.
TEST(DisplayRotation, ASmallerPictureIsCenteredAfterTheTurn) {
  const DisplayRotation r{90, 600, 400, 1024, 768};
  EXPECT_EQ(r.rotated_w(), 400);
  EXPECT_EQ(r.rotated_h(), 600);
  EXPECT_EQ(r.offset_x(), 312);
  EXPECT_EQ(r.offset_y(), 84);
  const RectI whole = r.ToPanel(RectI{0, 0, 600, 400});
  EXPECT_EQ(whole.x, 312);
  EXPECT_EQ(whole.y, 84);
  EXPECT_EQ(whole.w, 400);
  EXPECT_EQ(whole.h, 600);
}

// What the panel pixel samples is what the render pixel it holds would have
// sampled unrotated, through any texture mapping a layer brings: a crop, a
// buffer transform, a bottom-first GL texture. That is the whole contract of
// ToPanel(UvAffine).
TEST(DisplayRotation, APanelPixelSamplesWhatItsRenderPixelWould) {
  const UvAffine maps[] = {
      UvAffine{},
      FlipRows(UvAffine{}),
      UvForLayer({2, 1, 6, 5}, 9, 8, BufferTransform::k90),
      FlipRows(UvForLayer({0, 0, 4, 4}, 8, 8, BufferTransform::kFlipped270)),
  };
  for (const int deg : kAngles) {
    for (const UvAffine& uv : maps) {
      SCOPED_TRACE(deg);
      const DisplayRotation r = Filling(deg);
      const RectI layer{1, 0, 5, 3};
      const RectI panel = r.ToPanel(layer);
      const UvAffine turned = r.ToPanel(uv);
      for (int y = layer.y; y < layer.y + layer.h; ++y) {
        for (int x = layer.x; x < layer.x + layer.w; ++x) {
          const double s = (x + 0.5 - layer.x) / layer.w;
          const double t = (y + 0.5 - layer.y) / layer.h;
          const auto [px, py] = PanelPixel(r, x, y);
          const double sp = (px + 0.5 - panel.x) / panel.w;
          const double tp = (py + 0.5 - panel.y) / panel.h;
          EXPECT_NEAR(turned.U(sp, tp), uv.U(s, t), 1e-5);
          EXPECT_NEAR(turned.V(sp, tp), uv.V(s, t), 1e-5);
        }
      }
    }
  }
}

// The engine's form of the turn is ToPanel less the centering offset, for a
// picture that fills the panel and for one that does not.
TEST(DisplayRotation, TheTurnIsToPanelWithoutTheOffset) {
  for (const int deg : kAngles) {
    SCOPED_TRACE(deg);
    for (const DisplayRotation& r :
         {Filling(deg), DisplayRotation{deg, 6, 3, 10, 9}}) {
      const DisplayRotation::Affine m = r.Turn();
      for (int y = 0; y <= r.render_h; ++y) {
        for (int x = 0; x <= r.render_w; ++x) {
          double px = 0;
          double py = 0;
          r.ToPanel(x, y, &px, &py);
          EXPECT_DOUBLE_EQ(m.a * x + m.c * y + m.tx, px - r.offset_x());
          EXPECT_DOUBLE_EQ(m.b * x + m.d * y + m.ty, py - r.offset_y());
        }
      }
    }
  }
}

TEST(DisplayRotation, OnlyAPictureAsLargeAsThePanelFillsIt) {
  for (const int deg : kAngles) {
    SCOPED_TRACE(deg);
    EXPECT_TRUE(Filling(deg).fills_panel());
    EXPECT_FALSE((DisplayRotation{deg, 600, 400, 1024, 768}.fills_panel()));
  }
  // Turned, a 768x1024 picture fills a 1024x768 panel; upright it does not.
  EXPECT_TRUE((DisplayRotation{90, 768, 1024, 1024, 768}.fills_panel()));
  EXPECT_FALSE((DisplayRotation{180, 768, 1024, 1024, 768}.fills_panel()));
}

TEST(DisplayRotation, ZeroIsTheIdentity) {
  const DisplayRotation r = Filling(0);
  const UvAffine uv = UvForLayer({1, 2, 3, 4}, 8, 8, BufferTransform::k270);
  const UvAffine same = r.ToPanel(uv);
  EXPECT_FLOAT_EQ(same.a, uv.a);
  EXPECT_FLOAT_EQ(same.b, uv.b);
  EXPECT_FLOAT_EQ(same.c, uv.c);
  EXPECT_FLOAT_EQ(same.d, uv.d);
  EXPECT_FLOAT_EQ(same.tx, uv.tx);
  EXPECT_FLOAT_EQ(same.ty, uv.ty);
  const RectI rect = r.ToPanel(RectI{2, 1, 3, 2});
  EXPECT_EQ(rect.x, 2);
  EXPECT_EQ(rect.y, 1);
  EXPECT_EQ(rect.w, 3);
  EXPECT_EQ(rect.h, 2);
}
