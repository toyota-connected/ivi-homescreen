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
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include "gtest/gtest.h"

#include "backend/wayland_egl/gl_compositor.h"
#include "view/compositor_surface_interface.h"
#include "view/layer_geometry.h"

namespace {

constexpr int kW = 5;  // the image a layer shows, before any transform
constexpr int kH = 3;

// A distinct, exactly representable RGBA for image pixel `i`.
// Offset of pixel (x, y) in a row-major array of `n`-component pixels, `w`
// pixels wide. Every operand widened, so no index is formed in int.
size_t Px(int x, int y, int w, int n = 4) {
  return (static_cast<size_t>(y) * static_cast<size_t>(w) +
          static_cast<size_t>(x)) *
         static_cast<size_t>(n);
}

std::array<uint8_t, 4> ColorOf(int i) {
  return {static_cast<uint8_t>(20 + i * 10), static_cast<uint8_t>(200 - i * 7),
          static_cast<uint8_t>(i * 13 % 256), 255};
}

// Where a producer puts image pixel (x, y) in its buffer for `t`, written from
// the transform's definition (rotate counter-clockwise by 90-degree steps,
// mirroring first for FLIPPED).
std::pair<int, int> ProducerPlaces(BufferTransform t, int x, int y) {
  if (static_cast<uint32_t>(t) >= 4) {
    x = kW - 1 - x;
  }
  switch (static_cast<uint32_t>(t) % 4) {
    case 0:
      return {x, y};
    case 1:
      return {y, kW - 1 - x};
    case 2:
      return {kW - 1 - x, kH - 1 - y};
    default:
      return {kH - 1 - y, x};
  }
}

class LayerDrawGl : public ::testing::Test {
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
    display_ = get_platform_display == nullptr
                   ? EGL_NO_DISPLAY
                   : get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                          EGL_DEFAULT_DISPLAY, nullptr);
    if (display_ == EGL_NO_DISPLAY ||
        eglInitialize(display_, nullptr, nullptr) != EGL_TRUE) {
      display_ = EGL_NO_DISPLAY;
      GTEST_SKIP() << "surfaceless EGL display did not initialize";
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint config_attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                     EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                     EGL_NONE};
    EGLConfig config = nullptr;
    EGLint n = 0;
    if (eglChooseConfig(display_, config_attribs, &config, 1, &n) != EGL_TRUE ||
        n == 0) {
      GTEST_SKIP() << "no GLES2 config";
    }
    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, ctx_attribs);
    if (context_ == EGL_NO_CONTEXT ||
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_) !=
            EGL_TRUE) {
      GTEST_SKIP() << "no surfaceless GLES2 context";
    }
    compositor_ = std::make_unique<GlCompositor>(nullptr);  // quad path only
  }

  void TearDown() override {
    compositor_.reset();
    if (display_ != EGL_NO_DISPLAY) {
      eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (context_ != EGL_NO_CONTEXT) {
        eglDestroyContext(display_, context_);
      }
      eglTerminate(display_);
    }
  }

  // A w x h RGBA texture from rows of pixels, first row first (top-first, as a
  // dma-buf import is). Nearest filtering, so each destination pixel centre
  // reads exactly one source pixel.
  static GLuint MakeTexture(int w, int h, const std::vector<uint8_t>& rgba) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
  }

  // Draw `tex` into a fresh kW x kH target and return it top row first.
  std::vector<uint8_t> Draw(GLuint tex, const UvAffine& uv, bool opaque) {
    GLuint target = 0;
    glGenTextures(1, &target);
    glBindTexture(GL_TEXTURE_2D, target);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kW, kH, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           target, 0);
    EXPECT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    compositor_->CompositeLayerToFbo(fbo, tex, /*external=*/false, uv, 0, 0, kW,
                                     kH, /*blend=*/false, opaque);
    std::vector<uint8_t> bottom_up(static_cast<size_t>(kW) * kH * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, bottom_up.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &target);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    // GL reads bottom row first; flip to the top-first order Flutter lays out.
    std::vector<uint8_t> top_down(bottom_up.size());
    for (int y = 0; y < kH; ++y) {
      std::memcpy(&top_down[static_cast<size_t>(y) * kW * 4],
                  &bottom_up[static_cast<size_t>(kH - 1 - y) * kW * 4],
                  static_cast<size_t>(kW) * 4);
    }
    return top_down;
  }

  EGLDisplay display_{EGL_NO_DISPLAY};
  EGLContext context_{EGL_NO_CONTEXT};
  std::unique_ptr<GlCompositor> compositor_;
};

}  // namespace

// For each of the eight transforms: lay the image out as a producer would,
// draw it through the transform, and get the image back pixel for pixel.
TEST_F(LayerDrawGl, EveryTransformDrawsTheImageUpright) {
  for (uint32_t ti = 0; ti < 8; ++ti) {
    const auto t = static_cast<BufferTransform>(ti);
    SCOPED_TRACE(ti);
    const bool swap = TransformSwapsAxes(t);
    const int bw = swap ? kH : kW;
    const int bh = swap ? kW : kH;
    std::vector<uint8_t> buffer(Px(0, bh, bw), 0);
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x) {
        const auto [bx, by] = ProducerPlaces(t, x, y);
        const auto c = ColorOf(y * kW + x);
        std::memcpy(&buffer[Px(bx, by, bw)], c.data(), 4);
      }
    }
    const GLuint tex = MakeTexture(bw, bh, buffer);
    const auto out = Draw(
        tex,
        UvForLayer({}, static_cast<uint32_t>(bw), static_cast<uint32_t>(bh), t),
        /*opaque=*/false);
    glDeleteTextures(1, &tex);
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x) {
        const auto want = ColorOf(y * kW + x);
        const uint8_t* got = &out[static_cast<size_t>(y * kW + x) * 4];
        EXPECT_TRUE(std::memcmp(got, want.data(), 4) == 0)
            << "destination (" << x << ", " << y << "): got " << int(got[0])
            << "," << int(got[1]) << "," << int(got[2]) << " want "
            << int(want[0]) << "," << int(want[1]) << "," << int(want[2]);
      }
    }
  }
}

// A source crop shows only the cropped pixels, scaled to the destination.
TEST_F(LayerDrawGl, CropSelectsTheSourceRegion) {
  // A 2x-wide buffer: the image sits in its right half.
  std::vector<uint8_t> buffer(static_cast<size_t>(2) * kW * kH * 4, 0);
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      const auto c = ColorOf(y * kW + x);
      std::memcpy(&buffer[Px(kW + x, y, 2 * kW)], c.data(), 4);
    }
  }
  const GLuint tex = MakeTexture(2 * kW, kH, buffer);
  const auto out = Draw(
      tex, UvForLayer({kW, 0, kW, kH}, 2 * kW, kH, BufferTransform::kNormal),
      /*opaque=*/false);
  glDeleteTextures(1, &tex);
  for (int i = 0; i < kW * kH; ++i) {
    EXPECT_TRUE(std::memcmp(&out[static_cast<size_t>(i) * 4], ColorOf(i).data(),
                            4) == 0)
        << "pixel " << i;
  }
}

// An opaque layer draws alpha 1 whatever the buffer's alpha channel holds, as
// an XRGB buffer's does; a non-opaque one passes alpha through.
TEST_F(LayerDrawGl, OpaqueForcesAlphaToOne) {
  std::vector<uint8_t> buffer(static_cast<size_t>(kW) * kH * 4, 0);
  for (int i = 0; i < kW * kH; ++i) {
    buffer[static_cast<size_t>(i) * 4] = 0x40;
    buffer[static_cast<size_t>(i) * 4 + 3] = 0x10;  // undefined in XRGB
  }
  const GLuint tex = MakeTexture(kW, kH, buffer);
  const auto opaque = Draw(tex, UvAffine{}, /*opaque=*/true);
  const auto passed = Draw(tex, UvAffine{}, /*opaque=*/false);
  glDeleteTextures(1, &tex);
  for (int i = 0; i < kW * kH; ++i) {
    EXPECT_EQ(opaque[static_cast<size_t>(i) * 4 + 3], 0xff) << "pixel " << i;
    EXPECT_EQ(opaque[static_cast<size_t>(i) * 4], 0x40) << "pixel " << i;
    EXPECT_EQ(passed[static_cast<size_t>(i) * 4 + 3], 0x10) << "pixel " << i;
  }
}

namespace {

// A surface made of fixed textures and geometry, standing in for a plugin view
// submitted through ihs_pv_submit_layers.
class LayeredSurface : public ICompositorSurface {
 public:
  bool OnCreateBackingStore(const FlutterBackingStoreConfig*,
                            FlutterBackingStore*) override {
    return false;
  }
  bool OnCollectBackingStore(const FlutterBackingStore*) override {
    return true;
  }
  bool OnPresent(const FlutterLayer*) override { return true; }
  [[nodiscard]] FlutterPlatformViewIdentifier GetIdentifier() const override {
    return 1;
  }
  [[nodiscard]] size_t GetLayerCount() const override { return layers.size(); }
  [[nodiscard]] GlLayerTexture GetLayerGlTexture(size_t i) const override {
    return i < layers.size() ? layers[i] : GlLayerTexture{};
  }
  std::vector<GlLayerTexture> layers;
};

// Draw `surface` into a kW x kH target through CompositeSurfaceLayers and read
// it back top row first, for a target of either orientation.
std::vector<uint8_t> DrawSurface(GlCompositor& compositor,
                                 const ICompositorSurface& surface,
                                 bool target_top_first) {
  GLuint target = 0;
  glGenTextures(1, &target);
  glBindTexture(GL_TEXTURE_2D, target);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               nullptr);
  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         target, 0);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  compositor.CompositeSurfaceLayers(fbo, surface, {0, 0, kW, kH}, kH,
                                    target_top_first, /*blend_first=*/false);
  std::vector<uint8_t> rows(Px(0, kH, kW));
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(1, &target);
  if (target_top_first) {
    return rows;  // GL row 0 is the target's top row
  }
  std::vector<uint8_t> top_down(rows.size());
  for (int y = 0; y < kH; ++y) {
    std::memcpy(&top_down[Px(0, y, kW)], &rows[Px(0, kH - 1 - y, kW)],
                Px(kW, 0, 0));
  }
  return top_down;
}

}  // namespace

// Two layers of one view: a full-view background, and above it a 2 x 1 strip
// placed at view x = 4 -- half of it outside the view, so it is clipped -- and
// drawn from a buffer that holds it rotated a quarter turn. Right for both
// target orientations: a window's framebuffer, and an FBO scanned out top row
// first.
TEST_F(LayerDrawGl, SurfaceLayersStackClipAndOrientOnEitherTarget) {
  std::vector<uint8_t> bg(Px(0, kH, kW));
  for (int i = 0; i < kW * kH; ++i) {
    const auto c = ColorOf(i);
    std::memcpy(&bg[Px(i, 0, 0)], c.data(), 4);
  }
  // The strip shows colours A then B left to right. At IHS_TRANSFORM_90 the
  // producer rotated it counter-clockwise into a 1 x 2 buffer: B above A.
  const std::array<uint8_t, 4> kA = {250, 0, 0, 255};
  const std::array<uint8_t, 4> kB = {0, 0, 250, 255};
  std::vector<uint8_t> strip(8);
  std::memcpy(&strip[0], kB.data(), 4);
  std::memcpy(&strip[4], kA.data(), 4);

  const GLuint bg_tex = MakeTexture(kW, kH, bg);
  const GLuint strip_tex = MakeTexture(1, 2, strip);
  LayeredSurface surface;
  ICompositorSurface::GlLayerTexture base;
  base.name = bg_tex;
  base.width = kW;
  base.height = kH;
  base.top_first = true;
  ICompositorSurface::GlLayerTexture top;
  top.name = strip_tex;
  top.width = 1;
  top.height = 2;
  top.top_first = true;
  top.geometry.dst = {4, 1, 2, 1};
  top.geometry.transform = BufferTransform::k90;
  top.geometry.opaque = true;
  surface.layers = {base, top};

  for (const bool target_top_first : {false, true}) {
    SCOPED_TRACE(target_top_first ? "top-first target" : "window target");
    const auto out = DrawSurface(*compositor_, surface, target_top_first);
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x) {
        std::array<uint8_t, 4> want = ColorOf(y * kW + x);
        if (y == 1 && x == 4) {
          want = kA;  // the strip's left half; its right half is clipped
        }
        EXPECT_TRUE(std::memcmp(&out[Px(x, y, kW)], want.data(), 4) == 0)
            << "(" << x << ", " << y << "): got " << int(out[Px(x, y, kW)])
            << "," << int(out[Px(x, y, kW) + 1]) << ","
            << int(out[Px(x, y, kW) + 2]);
      }
    }
  }
  glDeleteTextures(1, &bg_tex);
  glDeleteTextures(1, &strip_tex);
}
