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

#ifndef SHELL_DISPLAY_SCALE_POLICY_H_
#define SHELL_DISPLAY_SCALE_POLICY_H_

#include <cstdint>

namespace ivi {

/**
 * @brief The buffer-at-physical / viewport-to-logical scaling policy: the
 * buffer is allocated at physical pixels, the wp_viewport destination is the
 * logical size, and the scene renders in logical units through a single
 * top-level canvas scale.
 *
 * Backend-agnostic (hence shell/display, not shell/wayland): any backend that
 * maps a logical size to physical pixels at a scale shares this rounding —
 * the Wayland path today, and a DRM/KMS panel that gains HiDPI/fractional
 * scaling later. It depends only on <cstdint>, so it is available in builds
 * that link no Wayland (or wayland-cxx-scanner) at all.
 *
 * Fractional scale is carried as @c scale_120 — the scale in 1/120 units,
 * exactly as wp_fractional_scale_v1.preferred_scale delivers it (unity is
 * 120). Integer wl_output.scale N maps to @c scale_120 = N * 120.
 *
 * Rounding is done in the integer domain — round-half-up of
 * `logical * scale_120 / 120` — NOT as `lround(logical * scale_120/120.0)`.
 * The two agree almost everywhere but disagree by one pixel wherever the
 * floating-point product lands just off an exact half (e.g. 100 px at
 * 1.025x: 100*123/120 = 102.5 → 103, but the double 123/120.0 * 100 is
 * 102.4999… → lround 102). One pixel is a real seam on a fractional-scaled
 * panel, so this is the single normative rounding for the project, kept
 * bit-identical to the wayland-cxx-scanner ScalePolicy reference via the
 * shared conformance vectors (see scale_policy_test).
 */
class ScalePolicy {
 public:
  /// wp_fractional_scale_v1 expresses unity scale as 120.
  static constexpr int32_t kUnityScale120 = 120;

  struct BufferSize {
    int32_t width = 0;
    int32_t height = 0;
  };

  /// Physical buffer dimension for a logical dimension at @p scale_120,
  /// rounded to the nearest whole pixel (half up) in the integer domain. A
  /// non-positive dimension yields 0; a non-positive @p scale_120 is treated
  /// as unity so a missing advertisement degrades to integer scale 1.
  [[nodiscard]] static constexpr int32_t ScaledDim(const int32_t logical,
                                                   const int32_t scale_120) {
    if (logical <= 0) {
      return 0;
    }
    const int32_t s = Normalize(scale_120);
    // 64-bit intermediate: 4096 * 360 (3x) fits int32, but a hostile logical
    // near INT32_MAX would overflow the 32-bit product; widen to be safe.
    const int64_t num = static_cast<int64_t>(logical) * s;
    return static_cast<int32_t>((num + kUnityScale120 / 2) / kUnityScale120);
  }

  /// Physical buffer size for a logical size at @p scale_120.
  [[nodiscard]] static constexpr BufferSize ToBuffer(const int32_t logical_w,
                                                     const int32_t logical_h,
                                                     const int32_t scale_120) {
    return BufferSize{ScaledDim(logical_w, scale_120),
                      ScaledDim(logical_h, scale_120)};
  }

  /// Canvas scale factor (logical → buffer pixels) as a double, for the
  /// engine pixel ratio / pointer scale / integer buffer_scale fallback —
  /// the FP-domain consumers that do not need pixel-exact rounding.
  [[nodiscard]] static constexpr double CanvasScale(const int32_t scale_120) {
    return static_cast<double>(Normalize(scale_120)) /
           static_cast<double>(kUnityScale120);
  }

 private:
  [[nodiscard]] static constexpr int32_t Normalize(const int32_t scale_120) {
    return scale_120 > 0 ? scale_120 : kUnityScale120;
  }
};

}  // namespace ivi

#endif  // SHELL_DISPLAY_SCALE_POLICY_H_
