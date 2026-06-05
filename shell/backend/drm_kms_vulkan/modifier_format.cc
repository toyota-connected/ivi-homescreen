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

#include "modifier_format.h"

#include <drm_fourcc.h>

#include <cstdio>

namespace drm_kms_vulkan {

namespace {

// Human-readable DRM-format-modifier vendor name (top 8 bits of the modifier).
const char* ModVendorName(uint8_t vendor) {
  switch (vendor) {
    case DRM_FORMAT_MOD_VENDOR_NONE:
      return "NONE";
    case DRM_FORMAT_MOD_VENDOR_INTEL:
      return "INTEL";
    case DRM_FORMAT_MOD_VENDOR_AMD:
      return "AMD";
    case DRM_FORMAT_MOD_VENDOR_NVIDIA:
      return "NVIDIA";
    case DRM_FORMAT_MOD_VENDOR_BROADCOM:
      return "BROADCOM";
    case DRM_FORMAT_MOD_VENDOR_ARM:
      return "ARM";
    case DRM_FORMAT_MOD_VENDOR_QCOM:
      return "QCOM";
    case DRM_FORMAT_MOD_VENDOR_VIVANTE:
      return "VIVANTE";
    case DRM_FORMAT_MOD_VENDOR_AMLOGIC:
      return "AMLOGIC";
    default:
      return "?";
  }
}

#ifdef AMD_FMT_MOD
const char* AmdTileVersionName(uint64_t v) {
  switch (v) {
    case AMD_FMT_MOD_TILE_VER_GFX9:
      return "GFX9";
    case AMD_FMT_MOD_TILE_VER_GFX10:
      return "GFX10";
    case AMD_FMT_MOD_TILE_VER_GFX10_RBPLUS:
      return "GFX10_RBPLUS";
    case AMD_FMT_MOD_TILE_VER_GFX11:
      return "GFX11";
    default:
      return "GFX?";
  }
}

// The displayable swizzle modes (the _X variants are the scanout-capable ones).
std::string AmdTileName(uint64_t t) {
  switch (t) {
    case AMD_FMT_MOD_TILE_GFX9_64K_S:
      return "64K_S";
    case AMD_FMT_MOD_TILE_GFX9_64K_D:
      return "64K_D";
    case AMD_FMT_MOD_TILE_GFX9_64K_S_X:
      return "64K_S_X";
    case AMD_FMT_MOD_TILE_GFX9_64K_D_X:
      return "64K_D_X";
    case AMD_FMT_MOD_TILE_GFX9_64K_R_X:
      return "64K_R_X";
    case AMD_FMT_MOD_TILE_GFX11_256K_R_X:
      return "256K_R_X";
    default:
      return "tile" + std::to_string(t);
  }
}
#endif  // AMD_FMT_MOD

#ifdef AFBC_FORMAT_MOD_BLOCK_SIZE_MASK
// Decode an ARM modifier. Rockchip VOP2 (rk3566 / rk3588) — and other Mali-fed
// displays — scan out ARM AFBC, so AFBC is the case worth breaking out; AFRC /
// other ARM types are named but left as hex. The AFBC "superblock" size + the
// YTR/SPARSE/TILED/etc. flags determine whether a Vulkan-exported buffer can be
// scanned out compressed.
std::string ArmDescribe(uint64_t mod) {
  // Type is bits [55:52]; AFBC is type 0. Its sub-mode is the low 52 bits
  // (compatible with both the pre- and post-AFRC encodings).
  if (((mod >> 52) & 0xfULL) != 0ULL) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "(0x%016llx)",
                  static_cast<unsigned long long>(mod));
    return std::string("ARM") + buf;  // AFRC / MISC — not decoded
  }
  const uint64_t m = mod & 0x000fffffffffffffULL;
  std::string s = "ARM(AFBC ";
  switch (m & AFBC_FORMAT_MOD_BLOCK_SIZE_MASK) {
    case AFBC_FORMAT_MOD_BLOCK_SIZE_16x16:
      s += "16x16";
      break;
    case AFBC_FORMAT_MOD_BLOCK_SIZE_32x8:
      s += "32x8";
      break;
    case AFBC_FORMAT_MOD_BLOCK_SIZE_64x4:
      s += "64x4";
      break;
    case AFBC_FORMAT_MOD_BLOCK_SIZE_32x8_64x4:
      s += "32x8_64x4";
      break;
    default:
      s += "blk?";
      break;
  }
  static const struct {
    uint64_t bit;
    const char* name;
  } kAfbcFlags[] = {
      {AFBC_FORMAT_MOD_YTR, " YTR"},       {AFBC_FORMAT_MOD_SPLIT, " SPLIT"},
      {AFBC_FORMAT_MOD_SPARSE, " SPARSE"}, {AFBC_FORMAT_MOD_CBR, " CBR"},
      {AFBC_FORMAT_MOD_TILED, " TILED"},   {AFBC_FORMAT_MOD_SC, " SC"},
      {AFBC_FORMAT_MOD_DB, " DB"},         {AFBC_FORMAT_MOD_BCH, " BCH"},
      {AFBC_FORMAT_MOD_USM, " USM"},
  };
  for (const auto& f : kAfbcFlags) {
    if ((m & f.bit) != 0ULL) {
      s += f.name;
    }
  }
  s += ')';
  return s;
}
#endif  // AFBC_FORMAT_MOD_BLOCK_SIZE_MASK

#ifdef DRM_FORMAT_MOD_BROADCOM_UIF
// Decode a Broadcom (Raspberry Pi V3D / vc4) modifier. The low 8 bits are the
// format code; the SAND variants carry a column-height parameter in the next
// 48 bits. UIF (Utile Interleaved Format) is V3D's tiled render layout — the
// non-LINEAR option a Pi can scan out (Broadcom has no AFBC-style lossless
// framebuffer compression, so this is tiling, not compression).
std::string BroadcomDescribe(uint64_t mod) {
  const uint64_t code = mod & 0xffULL;
  const uint64_t param = (mod >> 8) & ((1ULL << 48) - 1);  // SAND column height
  switch (code) {
    case 1:
      return "BROADCOM(VC4_T_TILED)";
    case 2:
      return "BROADCOM(SAND32 col=" + std::to_string(param) + ")";
    case 3:
      return "BROADCOM(SAND64 col=" + std::to_string(param) + ")";
    case 4:
      return "BROADCOM(SAND128 col=" + std::to_string(param) + ")";
    case 5:
      return "BROADCOM(SAND256 col=" + std::to_string(param) + ")";
    case 6:
      return "BROADCOM(UIF)";
    default: {
      char buf[24];
      std::snprintf(buf, sizeof(buf), "(0x%016llx)",
                    static_cast<unsigned long long>(mod));
      return std::string("BROADCOM") + buf;
    }
  }
}
#endif  // DRM_FORMAT_MOD_BROADCOM_UIF

}  // namespace

std::string DescribeModifier(uint64_t mod) {
  if (mod == DRM_FORMAT_MOD_LINEAR) {
    return "LINEAR";
  }
  if (mod == DRM_FORMAT_MOD_INVALID) {
    return "INVALID";
  }
  const auto vendor = static_cast<uint8_t>(mod >> 56);
#ifdef AMD_FMT_MOD
  if (vendor == DRM_FORMAT_MOD_VENDOR_AMD) {
    std::string s = "AMD(";
    s += AmdTileVersionName(AMD_FMT_MOD_GET(TILE_VERSION, mod));
    s += ' ';
    s += AmdTileName(AMD_FMT_MOD_GET(TILE, mod));
    s += " dcc=";
    s += std::to_string(AMD_FMT_MOD_GET(DCC, mod));
    if (AMD_FMT_MOD_GET(DCC_RETILE, mod) != 0u) {
      s += " retile";
    }
    if (AMD_FMT_MOD_GET(DCC_PIPE_ALIGN, mod) != 0u) {
      s += " pipe_align";
    }
    s += ')';
    return s;
  }
#endif  // AMD_FMT_MOD
#ifdef AFBC_FORMAT_MOD_BLOCK_SIZE_MASK
  if (vendor == DRM_FORMAT_MOD_VENDOR_ARM) {
    return ArmDescribe(mod);
  }
#endif  // AFBC_FORMAT_MOD_BLOCK_SIZE_MASK
#ifdef DRM_FORMAT_MOD_BROADCOM_UIF
  if (vendor == DRM_FORMAT_MOD_VENDOR_BROADCOM) {
    return BroadcomDescribe(mod);
  }
#endif  // DRM_FORMAT_MOD_BROADCOM_UIF
  char buf[24];
  std::snprintf(buf, sizeof(buf), "(0x%016llx)",
                static_cast<unsigned long long>(mod));
  return std::string(ModVendorName(vendor)) + buf;
}

}  // namespace drm_kms_vulkan
