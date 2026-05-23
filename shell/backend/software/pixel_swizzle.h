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

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__AVX2__) || defined(__SSSE3__) || defined(__SSE2__)
#include <immintrin.h>
#endif
#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

// Per-pixel format conversions for the software backend's sinks.
//
// Flutter's software surface_present_callback delivers Skia's
// kN32_SkColorType, which is endian-dependent:
//
//   little-endian host → kBGRA_8888 (memory bytes [B, G, R, A])
//   big-endian host    → kRGBA_8888 (memory bytes [R, G, B, A])
//
// All targets we ship to (x86_64, AArch64, ARMv7-A Linux, RISC-V
// Linux) are little-endian. The helpers below stay correct on BE
// hosts; the endian probe just selects which physical conversion is
// identity vs. byte-swap.
//
// SIMD acceleration is build-time gated on the toolchain's macros:
//
//   - x86 SSSE3 / AVX2  — _mm_shuffle_epi8 / _mm256_shuffle_epi8
//   - AArch64 NEON      — vqtbl1q_u8 (4 px / instr, 1-2 cyc latency
//                         on every modern µarch we ship to)
//   - ARMv7-A NEON      — vld4_u8 / vst4_u8 (Cortex-A8+; vqtbl1q
//                         is AArch64-only and the v7 TBL2 alternative
//                         is no quicker than the deinterleave path)
//   - scalar fallback   — anything else
//
// NEON version notes: byte-shuffle workloads peaked at ARMv8-A's
// vqtbl1q_u8 — later extensions (FP16, dotprod, I8MM, BF16) target
// arithmetic, not permutation. SVE/SVE2 only beats plain NEON on
// cores with VL > 128 (Neoverse V1 at 256, A64FX at 512). Embedded
// SVE2 cores (Cortex-A510/A710/A720) ship at VL=128, parity with
// NEON, so we don't carry an SVE2 path here. Add one if the target
// list grows to server-class silicon.

namespace ivi::swizzle {

#if !defined(__BYTE_ORDER__) || !defined(__ORDER_LITTLE_ENDIAN__)
#error "Cannot determine host endianness via __BYTE_ORDER__."
#endif
inline constexpr bool kHostIsLE = __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;

// Swap byte 0 and byte 2 within each 4-byte pixel. dst may equal src.
// Used to round-trip between BGRA-in-memory and RGBA-in-memory.
inline void SwapR_B(uint8_t* dst, const uint8_t* src, size_t pixels) {
  size_t i = 0;

#if defined(__AVX2__)
  {
    // _mm256_shuffle_epi8 shuffles within each 128-bit lane independently,
    // not across lanes. The 16-byte pattern is replicated so the upper
    // lane shuffles the same way as the lower. This is correct only
    // because pixels are 4 bytes and we advance by 8 px (32 B), keeping
    // each pixel inside one lane — never cross the 16-byte boundary.
    const __m256i mask =
        _mm256_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15,
                         2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
    for (; i + 8 <= pixels; i += 8) {
      __m256i v =
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i * 4));
      v = _mm256_shuffle_epi8(v, mask);
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i * 4), v);
    }
  }
#endif

#if defined(__SSSE3__)
  {
    const __m128i mask =
        _mm_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
    for (; i + 4 <= pixels; i += 4) {
      __m128i v =
          _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i * 4));
      v = _mm_shuffle_epi8(v, mask);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i * 4), v);
    }
  }
#elif defined(__aarch64__)
  {
    // AArch64 NEON: vqtbl1q_u8 is 1-2 cyc on Cortex-A55 / A76 / A78
    // / X-series; faster per byte than vld4q (4-8 cyc throughput) on
    // every modern µarch we ship to. 4 px / instruction.
    const uint8x16_t idx = {2,  1, 0, 3,  6,  5,  4,  7,
                            10, 9, 8, 11, 14, 13, 12, 15};
    for (; i + 4 <= pixels; i += 4) {
      uint8x16_t v = vld1q_u8(src + i * 4);
      v = vqtbl1q_u8(v, idx);
      vst1q_u8(dst + i * 4, v);
    }
  }
#elif defined(__ARM_NEON)
  {
    // ARMv7-A NEON (Cortex-A8 / A9 / A15): vqtbl1q_u8 is AArch64-only
    // and the v7 D-reg TBL2 alternative isn't faster than the
    // 4-channel deinterleave path, so use vld4_u8 / vst4_u8.
    // 8 px / iter; requires -mfpu=neon (or -march=armv7-a+neon).
    for (; i + 8 <= pixels; i += 8) {
      uint8x8x4_t v = vld4_u8(src + i * 4);
      uint8x8x4_t out{};
      out.val[0] = v.val[2];
      out.val[1] = v.val[1];
      out.val[2] = v.val[0];
      out.val[3] = v.val[3];
      vst4_u8(dst + i * 4, out);
    }
  }
#endif

  for (; i < pixels; ++i) {
    const uint8_t b0 = src[i * 4 + 0];
    const uint8_t b1 = src[i * 4 + 1];
    const uint8_t b2 = src[i * 4 + 2];
    const uint8_t b3 = src[i * 4 + 3];
    dst[i * 4 + 0] = b2;
    dst[i * 4 + 1] = b1;
    dst[i * 4 + 2] = b0;
    dst[i * 4 + 3] = b3;
  }
}

// Flutter → PAM TUPLTYPE RGB_ALPHA. PAM is bytes [R, G, B, A] in file
// order regardless of host endianness.
inline void FlutterToRGBA8888(uint8_t* dst,
                              const uint8_t* src,
                              const size_t pixels) {
  if constexpr (kHostIsLE) {
    SwapR_B(dst, src, pixels);
  } else {
    std::memcpy(dst, src, pixels * 4);
  }
}

// Single-pass copy + force byte 3 = 0xFF. LE-only callers.
// 0xFF000000 lands the 0xFF in byte 3 of each 4-byte pixel because
// LE DWORD packing places byte 3 in the high lane of the 32-bit word.
inline void CopyWithAlphaForce(uint8_t* dst,
                               const uint8_t* src,
                               size_t pixels) {
  size_t i = 0;

#if defined(__AVX2__)
  {
    const __m256i alpha = _mm256_set1_epi32(static_cast<int>(0xFF000000U));
    for (; i + 8 <= pixels; i += 8) {
      __m256i v =
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i * 4));
      v = _mm256_or_si256(v, alpha);
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i * 4), v);
    }
  }
#endif

#if defined(__SSE2__)
  {
    const __m128i alpha = _mm_set1_epi32(static_cast<int>(0xFF000000U));
    for (; i + 4 <= pixels; i += 4) {
      __m128i v =
          _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i * 4));
      v = _mm_or_si128(v, alpha);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i * 4), v);
    }
  }
#elif defined(__aarch64__) || defined(__ARM_NEON)
  {
    const uint8x16_t alpha = vreinterpretq_u8_u32(vdupq_n_u32(0xFF000000U));
    for (; i + 4 <= pixels; i += 4) {
      uint8x16_t v = vld1q_u8(src + i * 4);
      v = vorrq_u8(v, alpha);
      vst1q_u8(dst + i * 4, v);
    }
  }
#endif

  for (; i < pixels; ++i) {
    dst[i * 4 + 0] = src[i * 4 + 0];
    dst[i * 4 + 1] = src[i * 4 + 1];
    dst[i * 4 + 2] = src[i * 4 + 2];
    dst[i * 4 + 3] = 0xFF;
  }
}

// Flutter → DRM_FORMAT_XRGB8888, which per drm_fourcc.h is memory
// bytes [B, G, R, X] on both LE and BE hosts. On LE this matches
// Flutter's BGRA exactly: single-pass memcpy + alpha-force.
//
// Note: fbdev with red.offset=16 also lands at memory [B, G, R, X]
// on LE (the offsets describe bit positions inside the native-endian
// DWORD, and on LE byte 0 = LSB), so the LE branch is correct for
// both DRM XRGB and fbdev red@16. The two formats DIVERGE on BE:
// DRM XRGB is still [B, G, R, X], but fbdev red@16 becomes
// [X, R, G, B]. We don't ship BE targets; if that changes, fbdev
// needs its own helper rather than reusing this one.
inline void FlutterToBGRX8888(uint8_t* dst,
                              const uint8_t* src,
                              const size_t pixels) {
  if constexpr (kHostIsLE) {
    CopyWithAlphaForce(dst, src, pixels);
  } else {
    SwapR_B(dst, src, pixels);
    for (size_t i = 0; i < pixels; ++i) {
      dst[i * 4 + 3] = 0xFF;
    }
  }
}

}  // namespace ivi::swizzle
