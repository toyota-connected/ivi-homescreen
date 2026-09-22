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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm_fourcc.h>

#include "gtest/gtest.h"

#include "egl_dmabuf_import.h"

namespace {

// What the display itself lists for a format, straight from the extension.
struct Listed {
  std::vector<uint64_t> modifiers;
  std::vector<bool> external_only;
};

class EglImportModifiers : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (client == nullptr ||
        std::strstr(client, "EGL_MESA_platform_surfaceless") == nullptr) {
      GTEST_SKIP() << "no EGL_MESA_platform_surfaceless";
    }
    auto get_platform_display =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (get_platform_display == nullptr) {
      GTEST_SKIP() << "no eglGetPlatformDisplayEXT";
    }
    display_ = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                    EGL_DEFAULT_DISPLAY, nullptr);
    if (display_ == EGL_NO_DISPLAY ||
        eglInitialize(display_, nullptr, nullptr) != EGL_TRUE) {
      display_ = EGL_NO_DISPLAY;
      GTEST_SKIP() << "surfaceless EGL display did not initialize";
    }
    const char* exts = eglQueryString(display_, EGL_EXTENSIONS);
    if (exts == nullptr ||
        std::strstr(exts, "EGL_EXT_image_dma_buf_import_modifiers") ==
            nullptr) {
      GTEST_SKIP() << "no EGL_EXT_image_dma_buf_import_modifiers";
    }
    if (!importer_.Init(display_)) {
      GTEST_SKIP() << "no EGL_EXT_image_dma_buf_import";
    }
  }

  void TearDown() override {
    if (display_ != EGL_NO_DISPLAY) {
      eglTerminate(display_);
    }
  }

  [[nodiscard]] Listed List(uint32_t fourcc) const {
    Listed out;
    auto query = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(
        eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    EGLint n = 0;
    if (query == nullptr ||
        query(display_, static_cast<EGLint>(fourcc), 0, nullptr, nullptr, &n) !=
            EGL_TRUE ||
        n <= 0) {
      return out;
    }
    std::vector<EGLuint64KHR> mods(static_cast<size_t>(n));
    std::vector<EGLBoolean> ext(static_cast<size_t>(n));
    EXPECT_EQ(query(display_, static_cast<EGLint>(fourcc), n, mods.data(),
                    ext.data(), &n),
              EGL_TRUE);
    for (EGLint i = 0; i < n; ++i) {
      out.modifiers.push_back(mods[static_cast<size_t>(i)]);
      out.external_only.push_back(ext[static_cast<size_t>(i)] == EGL_TRUE);
    }
    return out;
  }

  EGLDisplay display_{EGL_NO_DISPLAY};
  EglDmabufImporter importer_;
};

}  // namespace

// For each packed RGB format the capability query offers: only modifiers the
// display lists, none it restricts to GL_TEXTURE_EXTERNAL_OES (the importer
// binds these as GL_TEXTURE_2D), no INVALID, no repeats, LINEAR last.
TEST_F(EglImportModifiers, RgbOfferIsImportableAsTexture2dWithLinearLast) {
  for (const uint32_t fourcc : {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888,
                                DRM_FORMAT_XBGR8888, DRM_FORMAT_ABGR8888}) {
    SCOPED_TRACE(fourcc);
    const Listed listed = List(fourcc);
    const std::vector<uint64_t> offered = importer_.ImportableModifiers(fourcc);

    std::set<uint64_t> seen;
    for (size_t i = 0; i < offered.size(); ++i) {
      const uint64_t m = offered[i];
      EXPECT_NE(m, DRM_FORMAT_MOD_INVALID);
      EXPECT_TRUE(seen.insert(m).second)
          << "repeated modifier 0x" << std::hex << m;
      if (m == DRM_FORMAT_MOD_LINEAR) {
        EXPECT_EQ(i, offered.size() - 1) << "LINEAR must come last";
      }
      const auto it =
          std::find(listed.modifiers.begin(), listed.modifiers.end(), m);
      ASSERT_NE(it, listed.modifiers.end())
          << "0x" << std::hex << m << " is not one the display lists";
      EXPECT_FALSE(listed.external_only[static_cast<size_t>(
          it - listed.modifiers.begin())])
          << "0x" << std::hex << m << " is external-only";
    }

    // And nothing usable is left out.
    size_t usable = 0;
    for (size_t i = 0; i < listed.modifiers.size(); ++i) {
      if (!listed.external_only[i] &&
          listed.modifiers[i] != DRM_FORMAT_MOD_INVALID) {
        ++usable;
      }
    }
    EXPECT_EQ(offered.size(), usable);
  }
}

// A format the display does not list yields nothing to offer, rather than a
// guess.
TEST_F(EglImportModifiers, UnknownFormatOffersNothing) {
  EXPECT_TRUE(
      importer_.ImportableModifiers(fourcc_code('Z', 'Z', 'Z', 'Z')).empty());
}

// Without Init there is no display to ask.
TEST(EglImportModifiersNoDisplay, UninitializedImporterOffersNothing) {
  const EglDmabufImporter importer;
  EXPECT_TRUE(importer.ImportableModifiers(DRM_FORMAT_XRGB8888).empty());
}
