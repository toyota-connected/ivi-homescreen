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

#include <array>
#include <cstdint>
#include <vector>

#include "gtest/gtest.h"

#include "backend/software/pixel_swizzle.h"

namespace {

// Pack a single BGRA-in-memory pixel into the RGB565 word using the
// scalar reference formula. Used as the bit-exact ground truth.
uint16_t Pack565(uint8_t b, uint8_t g, uint8_t r) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) |
                               (b >> 3));
}

// Build a pixel buffer of @p n pixels, each set to [B, G, R, A] in
// memory order (Flutter's LE format).
std::vector<uint8_t> MakeBuf(size_t n,
                             uint8_t b,
                             uint8_t g,
                             uint8_t r,
                             uint8_t a) {
  std::vector<uint8_t> buf(n * 4);
  for (size_t i = 0; i < n; ++i) {
    buf[i * 4 + 0] = b;
    buf[i * 4 + 1] = g;
    buf[i * 4 + 2] = r;
    buf[i * 4 + 3] = a;
  }
  return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// FlutterToRGB565 — truncating pack.
// ---------------------------------------------------------------------------

TEST(PixelSwizzle, RGB565_AllBlack) {
  auto src = MakeBuf(8, 0, 0, 0, 0xFF);
  std::array<uint16_t, 8> dst{};
  ivi::swizzle::FlutterToRGB565(dst.data(), src.data(), 8);
  for (uint16_t v : dst) {
    EXPECT_EQ(v, 0x0000);
  }
}

TEST(PixelSwizzle, RGB565_AllWhite) {
  auto src = MakeBuf(8, 0xFF, 0xFF, 0xFF, 0xFF);
  std::array<uint16_t, 8> dst{};
  ivi::swizzle::FlutterToRGB565(dst.data(), src.data(), 8);
  for (uint16_t v : dst) {
    EXPECT_EQ(v, 0xFFFF);
  }
}

TEST(PixelSwizzle, RGB565_PureRed) {
  auto src = MakeBuf(8, 0x00, 0x00, 0xFF, 0xFF);
  std::array<uint16_t, 8> dst{};
  ivi::swizzle::FlutterToRGB565(dst.data(), src.data(), 8);
  for (uint16_t v : dst) {
    EXPECT_EQ(v, 0xF800);
  }
}

TEST(PixelSwizzle, RGB565_PureGreen) {
  auto src = MakeBuf(8, 0x00, 0xFF, 0x00, 0xFF);
  std::array<uint16_t, 8> dst{};
  ivi::swizzle::FlutterToRGB565(dst.data(), src.data(), 8);
  for (uint16_t v : dst) {
    EXPECT_EQ(v, 0x07E0);
  }
}

TEST(PixelSwizzle, RGB565_PureBlue) {
  auto src = MakeBuf(8, 0xFF, 0x00, 0x00, 0xFF);
  std::array<uint16_t, 8> dst{};
  ivi::swizzle::FlutterToRGB565(dst.data(), src.data(), 8);
  for (uint16_t v : dst) {
    EXPECT_EQ(v, 0x001F);
  }
}

TEST(PixelSwizzle, RGB565_MidGray) {
  // 128 → R5 = 16 (0x10), G6 = 32 (0x20), B5 = 16 (0x10).
  // Packed: (0x80 & 0xF8) << 8 | (0x80 & 0xFC) << 3 | 0x10 = 0x8410.
  auto src = MakeBuf(8, 0x80, 0x80, 0x80, 0xFF);
  std::array<uint16_t, 8> dst{};
  ivi::swizzle::FlutterToRGB565(dst.data(), src.data(), 8);
  for (uint16_t v : dst) {
    EXPECT_EQ(v, 0x8410);
  }
}

TEST(PixelSwizzle, RGB565_AlphaIsDiscarded) {
  // Alpha varies but RGB565 has no alpha channel — packed value must
  // be identical regardless of A.
  auto a0 = MakeBuf(4, 0x40, 0x80, 0xC0, 0x00);
  auto aFF = MakeBuf(4, 0x40, 0x80, 0xC0, 0xFF);
  std::array<uint16_t, 4> dst0{};
  std::array<uint16_t, 4> dstFF{};
  ivi::swizzle::FlutterToRGB565(dst0.data(), a0.data(), 4);
  ivi::swizzle::FlutterToRGB565(dstFF.data(), aFF.data(), 4);
  for (size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(dst0[i], dstFF[i]);
    EXPECT_EQ(dst0[i], Pack565(0x40, 0x80, 0xC0));
  }
}

// 17 pixels forces the NEON 16-px block + 1-px scalar tail. Exercises
// the boundary handoff between vector and scalar paths.
TEST(PixelSwizzle, RGB565_ScalarTail_17px) {
  std::vector<uint8_t> src(17 * 4);
  // Per-pixel varied colour so a wrong loop bound shows up.
  for (size_t i = 0; i < 17; ++i) {
    src[i * 4 + 0] = static_cast<uint8_t>(i * 15);      // B
    src[i * 4 + 1] = static_cast<uint8_t>(i * 11 + 4);  // G
    src[i * 4 + 2] = static_cast<uint8_t>(i * 7 + 20);  // R
    src[i * 4 + 3] = 0xFF;
  }
  std::array<uint16_t, 17> dst{};
  ivi::swizzle::FlutterToRGB565(dst.data(), src.data(), 17);
  for (size_t i = 0; i < 17; ++i) {
    EXPECT_EQ(dst[i], Pack565(src[i * 4 + 0], src[i * 4 + 1], src[i * 4 + 2]))
        << "pixel " << i;
  }
}

// 23 pixels = 16-px NEON block + 7-px scalar tail (the 8-px NEON
// path skips because 8 > 7 remaining). Forces the cleanup loop.
TEST(PixelSwizzle, RGB565_ScalarTail_23px) {
  std::vector<uint8_t> src(23 * 4);
  for (size_t i = 0; i < 23; ++i) {
    src[i * 4 + 0] = static_cast<uint8_t>(0xFF - i * 5);
    src[i * 4 + 1] = static_cast<uint8_t>(i * 3 + 100);
    src[i * 4 + 2] = static_cast<uint8_t>(i * 13);
    src[i * 4 + 3] = 0x80;
  }
  std::array<uint16_t, 23> dst{};
  ivi::swizzle::FlutterToRGB565(dst.data(), src.data(), 23);
  for (size_t i = 0; i < 23; ++i) {
    EXPECT_EQ(dst[i], Pack565(src[i * 4 + 0], src[i * 4 + 1], src[i * 4 + 2]))
        << "pixel " << i;
  }
}

// 1 pixel — entirely scalar (no NEON path). Catches a scalar-only
// regression that the bulk-loop tests would miss.
TEST(PixelSwizzle, RGB565_ScalarOnly_1px) {
  std::array<uint8_t, 4> src{0x12, 0x34, 0x56, 0xFF};
  std::array<uint16_t, 1> dst{};
  ivi::swizzle::FlutterToRGB565(dst.data(), src.data(), 1);
  EXPECT_EQ(dst[0], Pack565(0x12, 0x34, 0x56));
}

// ---------------------------------------------------------------------------
// FlutterToRGB565_BayerDither — saturating ordered dither.
// ---------------------------------------------------------------------------

TEST(PixelSwizzle, BayerDither_AllBlack_StaysBlack) {
  // 0 + bayer (0..15) >> 1 = 0..7, then truncate (>>3) = 0. G chan:
  // 0 + bayer >> 2 = 0..3, >> 2 = 0. Output must be all-zero.
  auto src = MakeBuf(8, 0, 0, 0, 0);
  for (size_t y = 0; y < 4; ++y) {
    std::array<uint16_t, 8> dst{};
    ivi::swizzle::FlutterToRGB565_BayerDither(dst.data(), src.data(), 8, y);
    for (uint16_t v : dst) {
      EXPECT_EQ(v, 0x0000) << "row y=" << y;
    }
  }
}

TEST(PixelSwizzle, BayerDither_AllWhite_StaysWhite) {
  // Saturating add: 255 + offset clamps to 255, then truncate gives
  // 31 / 63 / 31. Output must be 0xFFFF.
  auto src = MakeBuf(8, 0xFF, 0xFF, 0xFF, 0xFF);
  for (size_t y = 0; y < 4; ++y) {
    std::array<uint16_t, 8> dst{};
    ivi::swizzle::FlutterToRGB565_BayerDither(dst.data(), src.data(), 8, y);
    for (uint16_t v : dst) {
      EXPECT_EQ(v, 0xFFFF) << "row y=" << y;
    }
  }
}

TEST(PixelSwizzle, BayerDither_NearWhite_DoesNotWrap) {
  // The property to check is "no wrap-around" — if vqadd were
  // mis-specified as wrapping, 250 + 7 = 257 mod 256 = 1 would
  // collapse R5/B5 to 0 (black speckle in highlights). Verify each
  // pixel's quantized R5 / B5 are 30 or 31, never 0.
  auto src = MakeBuf(8, 250, 250, 250, 0xFF);
  std::array<uint16_t, 8> dst{};
  // y=3 has bayer values that include 15 — the largest offset.
  ivi::swizzle::FlutterToRGB565_BayerDither(dst.data(), src.data(), 8, 3);
  for (uint16_t v : dst) {
    const uint16_t r5 = (v >> 11) & 0x1F;
    const uint16_t b5 = v & 0x1F;
    EXPECT_GE(r5, 30u) << "0x" << std::hex << v;
    EXPECT_GE(b5, 30u) << "0x" << std::hex << v;
  }
}

TEST(PixelSwizzle, BayerDither_RowSelectsCorrectMatrix) {
  // Pick a source value at a B5 truncation boundary so the dither
  // offset actually flips a bit. B = 0x07: scalar truncates to
  // B5=0. + offset of 1 → 0x08 → B5=1. The 4x4 Bayer matrix has
  // different per-column patterns per row, so the resulting B5
  // sequence across columns must differ between row 0 and row 1.
  auto src = MakeBuf(16, 0x07, 0x07, 0x07, 0xFF);
  std::array<uint16_t, 16> y0{};
  std::array<uint16_t, 16> y1{};
  ivi::swizzle::FlutterToRGB565_BayerDither(y0.data(), src.data(), 16, 0);
  ivi::swizzle::FlutterToRGB565_BayerDither(y1.data(), src.data(), 16, 1);
  bool rows_differ = false;
  for (size_t i = 0; i < 16; ++i) {
    if (y0[i] != y1[i]) {
      rows_differ = true;
      break;
    }
  }
  EXPECT_TRUE(rows_differ);
}

TEST(PixelSwizzle, BayerDither_ScalarTail_17px) {
  std::vector<uint8_t> src(17 * 4);
  for (size_t i = 0; i < 17; ++i) {
    src[i * 4 + 0] = static_cast<uint8_t>(0x40 + i);
    src[i * 4 + 1] = static_cast<uint8_t>(0x60 + i);
    src[i * 4 + 2] = static_cast<uint8_t>(0x80 + i);
    src[i * 4 + 3] = 0xFF;
  }
  // Run with two different y values to catch row indexing in the tail.
  for (size_t y : {0u, 1u, 2u, 3u}) {
    std::array<uint16_t, 17> dst{};
    ivi::swizzle::FlutterToRGB565_BayerDither(dst.data(), src.data(), 17, y);
    // Spot-check the tail pixel (i=16). Bayer matrix row y, col (16 & 3) = 0.
    // bayer[y][0] = {0, 12, 3, 15}.
    constexpr uint8_t kBayer[4] = {0, 12, 3, 15};
    const uint8_t off = kBayer[y];
    const uint16_t b_dith = std::min(255, 0x40 + 16 + (off >> 1));
    const uint16_t g_dith = std::min(255, 0x60 + 16 + (off >> 2));
    const uint16_t r_dith = std::min(255, 0x80 + 16 + (off >> 1));
    EXPECT_EQ(dst[16], Pack565(static_cast<uint8_t>(b_dith),
                               static_cast<uint8_t>(g_dith),
                               static_cast<uint8_t>(r_dith)))
        << "tail pixel for y=" << y;
  }
}

// ---------------------------------------------------------------------------
// SwapR_B — byte 0 ↔ byte 2 within each pixel. Used by FlutterToRGBA8888.
// ---------------------------------------------------------------------------

TEST(PixelSwizzle, SwapR_B_Identity_Roundtrip) {
  // Apply twice → original.
  std::vector<uint8_t> src(32 * 4);
  for (size_t i = 0; i < src.size(); ++i) {
    src[i] = static_cast<uint8_t>(i * 7 + 13);
  }
  std::vector<uint8_t> tmp(src.size());
  std::vector<uint8_t> out(src.size());
  ivi::swizzle::SwapR_B(tmp.data(), src.data(), 32);
  ivi::swizzle::SwapR_B(out.data(), tmp.data(), 32);
  EXPECT_EQ(src, out);
}

TEST(PixelSwizzle, SwapR_B_PreservesGreenAndAlpha) {
  std::vector<uint8_t> src(8 * 4);
  for (size_t i = 0; i < 8; ++i) {
    src[i * 4 + 0] = 0x10;  // B
    src[i * 4 + 1] = 0x20;  // G
    src[i * 4 + 2] = 0x30;  // R
    src[i * 4 + 3] = 0x40;  // A
  }
  std::vector<uint8_t> dst(8 * 4);
  ivi::swizzle::SwapR_B(dst.data(), src.data(), 8);
  for (size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(dst[i * 4 + 0], 0x30);  // R now at byte 0
    EXPECT_EQ(dst[i * 4 + 1], 0x20);  // G unchanged
    EXPECT_EQ(dst[i * 4 + 2], 0x10);  // B now at byte 2
    EXPECT_EQ(dst[i * 4 + 3], 0x40);  // A unchanged
  }
}

// 17 pixels — exercises x86 SSSE3 (4-px block) and AVX2 (8-px block)
// or NEON paths, plus the scalar tail.
TEST(PixelSwizzle, SwapR_B_ScalarTail_17px) {
  std::vector<uint8_t> src(17 * 4);
  for (size_t i = 0; i < src.size(); ++i) {
    src[i] = static_cast<uint8_t>(i);
  }
  std::vector<uint8_t> dst(17 * 4);
  ivi::swizzle::SwapR_B(dst.data(), src.data(), 17);
  for (size_t i = 0; i < 17; ++i) {
    EXPECT_EQ(dst[i * 4 + 0], src[i * 4 + 2]);
    EXPECT_EQ(dst[i * 4 + 1], src[i * 4 + 1]);
    EXPECT_EQ(dst[i * 4 + 2], src[i * 4 + 0]);
    EXPECT_EQ(dst[i * 4 + 3], src[i * 4 + 3]);
  }
}

// ---------------------------------------------------------------------------
// FlutterToBGRX8888 — memcpy + alpha-force on LE.
// ---------------------------------------------------------------------------

TEST(PixelSwizzle, BGRX8888_ForcesAlphaTo0xFF) {
  auto src = MakeBuf(8, 0x40, 0x80, 0xC0, 0x00);
  std::vector<uint8_t> dst(8 * 4);
  ivi::swizzle::FlutterToBGRX8888(dst.data(), src.data(), 8);
  for (size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(dst[i * 4 + 0], 0x40);  // B preserved
    EXPECT_EQ(dst[i * 4 + 1], 0x80);  // G preserved
    EXPECT_EQ(dst[i * 4 + 2], 0xC0);  // R preserved
    EXPECT_EQ(dst[i * 4 + 3], 0xFF);  // X forced
  }
}

TEST(PixelSwizzle, BGRX8888_ScalarTail_17px) {
  std::vector<uint8_t> src(17 * 4);
  for (size_t i = 0; i < src.size(); ++i) {
    src[i] = static_cast<uint8_t>(i * 5 + 7);
  }
  std::vector<uint8_t> dst(17 * 4);
  ivi::swizzle::FlutterToBGRX8888(dst.data(), src.data(), 17);
  for (size_t i = 0; i < 17; ++i) {
    EXPECT_EQ(dst[i * 4 + 0], src[i * 4 + 0]);
    EXPECT_EQ(dst[i * 4 + 1], src[i * 4 + 1]);
    EXPECT_EQ(dst[i * 4 + 2], src[i * 4 + 2]);
    EXPECT_EQ(dst[i * 4 + 3], 0xFF);
  }
}
