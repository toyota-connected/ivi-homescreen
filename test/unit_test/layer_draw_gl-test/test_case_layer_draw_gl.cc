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
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include "gtest/gtest.h"

#include "backend/wayland_egl/gl_caps.h"
#include "backend/wayland_egl/gl_compositor.h"
#include "view/compositor_surface_interface.h"
#include "view/display_rotation.h"
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
  virtual EGLint ClientVersion() const { return 2; }
  virtual const GlCaps* ProbeCaps() { return nullptr; }

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
    const EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE,
        ClientVersion() >= 3 ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint n = 0;
    if (eglChooseConfig(display_, config_attribs, &config, 1, &n) != EGL_TRUE ||
        n == 0) {
      GTEST_SKIP() << "no GLES" << ClientVersion() << " config";
    }
    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, ClientVersion(),
                                  EGL_NONE};
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, ctx_attribs);
    if (context_ == EGL_NO_CONTEXT ||
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_) !=
            EGL_TRUE) {
      GTEST_SKIP() << "no surfaceless GLES" << ClientVersion() << " context";
    }
    compositor_ = std::make_unique<GlCompositor>(ProbeCaps());
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
  // dma-buf import is). Nearest filtering, so each destination pixel center
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
  // `in_frame` wraps the draw in BeginFrame/EndFrame, the batched path a
  // backend takes when it composites a frame of layers.
  std::vector<uint8_t> Draw(GLuint tex,
                            const UvAffine& uv,
                            bool opaque,
                            bool in_frame = false) {
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
    if (in_frame) {
      compositor_->BeginFrame();
    }
    compositor_->CompositeLayerToFbo(fbo, tex, /*external=*/false, uv, 0, 0, kW,
                                     kH, /*blend=*/false, opaque);
    if (in_frame) {
      compositor_->EndFrame();
    }
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

class LayerDrawGlEs3 : public LayerDrawGl {
 protected:
  EGLint ClientVersion() const override { return 3; }
  const GlCaps* ProbeCaps() override {
    caps_.Probe();
    return &caps_;
  }

  // Leave a sampler with a mipmap minification filter on unit 0, as Flutter's
  // renderer can, then draw a texture that has only level 0.
  void DrawWithMipmapSamplerOnUnit0(bool in_frame) {
    using GenSamplersFn = void(GL_APIENTRY*)(GLsizei, GLuint*);
    using SamplerParameteriFn = void(GL_APIENTRY*)(GLuint, GLenum, GLint);
    using DeleteSamplersFn = void(GL_APIENTRY*)(GLsizei, const GLuint*);
    const auto gen_samplers =
        reinterpret_cast<GenSamplersFn>(eglGetProcAddress("glGenSamplers"));
    const auto sampler_parameteri = reinterpret_cast<SamplerParameteriFn>(
        eglGetProcAddress("glSamplerParameteri"));
    const auto delete_samplers = reinterpret_cast<DeleteSamplersFn>(
        eglGetProcAddress("glDeleteSamplers"));
    ASSERT_NE(gen_samplers, nullptr);
    ASSERT_NE(sampler_parameteri, nullptr);
    ASSERT_NE(delete_samplers, nullptr);
    ASSERT_NE(caps_.bind_sampler, nullptr) << "glBindSampler did not resolve";

    std::vector<uint8_t> image(Px(0, kH, kW));
    for (int i = 0; i < kW * kH; ++i) {
      const auto c = ColorOf(i);
      std::memcpy(&image[Px(i, 0, 0)], c.data(), 4);
    }
    const GLuint tex = MakeTexture(kW, kH, image);  // one level, no mipmaps

    GLuint sampler = 0;
    gen_samplers(1, &sampler);
    sampler_parameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glActiveTexture(GL_TEXTURE0);
    caps_.bind_sampler(0, sampler);

    const auto out = Draw(tex, UvForLayer({}, kW, kH, BufferTransform::kNormal),
                          /*opaque=*/false, in_frame);

    caps_.bind_sampler(0, 0);
    delete_samplers(1, &sampler);
    glDeleteTextures(1, &tex);
    for (int i = 0; i < kW * kH; ++i) {
      const auto want = ColorOf(i);
      const uint8_t* got = &out[Px(i, 0, 0)];
      EXPECT_TRUE(std::memcmp(got, want.data(), 4) == 0)
          << "pixel " << i << ": got " << int(got[0]) << "," << int(got[1])
          << "," << int(got[2]) << " want " << int(want[0]) << ","
          << int(want[1]) << "," << int(want[2]);
    }
  }

  GlCaps caps_;
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
// A producer that renders bottom-up (#718) says so on the frame, and the
// compositor samples the texture flipped rather than the producer blitting it
// upside down into its ring every frame. The same texture drawn both ways must
// come out mirrored, which is the whole of what the flag buys.
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
    if (i == 0) {
      layer0_taken++;
    }
    return i < layers.size() ? layers[i] : GlLayerTexture{};
  }
  std::vector<GlLayerTexture> layers;
  // How often layer 0 was asked for, which for a real view imports a frame.
  mutable int layer0_taken{0};
};

// Draw `surface` into a kW x kH target through CompositeSurfaceLayers and read
// it back top row first, for a target of either orientation.
std::vector<uint8_t> DrawSurface(
    GlCompositor& compositor,
    const ICompositorSurface& surface,
    bool target_top_first,
    const ICompositorSurface::GlLayerTexture* layer0 = nullptr,
    const RectI* clip = nullptr,
    float alpha = 1.0f) {
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
  compositor.CompositeSurfaceLayers(
      fbo, surface, {0, 0, kW, kH}, kH, target_top_first, /*blend_first=*/false,
      nullptr, layer0, /*rotation=*/nullptr, clip, alpha);
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
  // The strip shows colors A then B left to right. At IHS_TRANSFORM_90 the
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

TEST_F(LayerDrawGl, ABottomUpSourceIsSampledFlipped) {
  std::vector<uint8_t> img(Px(0, kH, kW));
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      std::memcpy(&img[Px(x, y, kW)], ColorOf(y * kW + x).data(), 4);
    }
  }
  const GLuint tex = MakeTexture(kW, kH, img);

  auto draw = [&](const bool top_first) {
    LayeredSurface surface;
    ICompositorSurface::GlLayerTexture layer;
    layer.name = tex;
    layer.width = kW;
    layer.height = kH;
    layer.top_first = top_first;
    surface.layers = {layer};
    return DrawSurface(*compositor_, surface, /*target_top_first=*/true);
  };
  const auto top_down = draw(true);
  const auto bottom_up = draw(false);
  ASSERT_EQ(top_down.size(), bottom_up.size());

  // Rows differ, so a mirror is observable rather than vacuously true.
  ASSERT_NE(std::memcmp(top_down.data(), bottom_up.data(), top_down.size()), 0)
      << "the two orientations drew the same pixels; the flag did nothing";
  for (int y = 0; y < kH; ++y) {
    EXPECT_EQ(std::memcmp(&bottom_up[Px(0, y, kW)],
                          &top_down[Px(0, kH - 1 - y, kW)], Px(kW, 0, 0)),
              0)
        << "row " << y << " of the bottom-up draw is not row " << (kH - 1 - y)
        << " of the top-first one";
  }
}

// Layer 0 handed in by a caller that already took it is drawn as is, and not
// asked for again: for a real view that would import a newer frame over the
// one taken, which nothing then draws or releases.
TEST_F(LayerDrawGl, ALayerZeroAlreadyTakenIsDrawnNotTakenAgain) {
  const std::array<uint8_t, 4> kTaken = {250, 0, 0, 255};
  const std::array<uint8_t, 4> kNewer = {0, 0, 250, 255};
  std::vector<uint8_t> taken_px(Px(0, kH, kW));
  std::vector<uint8_t> newer_px(Px(0, kH, kW));
  for (int i = 0; i < kW * kH; ++i) {
    std::memcpy(&taken_px[Px(i, 0, 0)], kTaken.data(), 4);
    std::memcpy(&newer_px[Px(i, 0, 0)], kNewer.data(), 4);
  }
  const GLuint taken_tex = MakeTexture(kW, kH, taken_px);
  const GLuint newer_tex = MakeTexture(kW, kH, newer_px);
  ICompositorSurface::GlLayerTexture taken;
  taken.name = taken_tex;
  taken.width = kW;
  taken.height = kH;
  taken.top_first = true;
  taken.geometry.src = {0, 0, kW, kH};  // cropped to all of it: layered path
  ICompositorSurface::GlLayerTexture newer = taken;
  newer.name = newer_tex;
  LayeredSurface surface;
  surface.layers = {newer};

  const auto out =
      DrawSurface(*compositor_, surface, /*target_top_first=*/true, &taken);
  EXPECT_EQ(surface.layer0_taken, 0) << "layer 0 was taken a second time";
  EXPECT_EQ(std::memcmp(&out[Px(kW / 2, kH / 2, kW)], kTaken.data(), 4), 0)
      << "drew a frame other than the one handed in";
  glDeleteTextures(1, &taken_tex);
  glDeleteTextures(1, &newer_tex);
}

TEST_F(LayerDrawGlEs3, SamplerLeftOnUnit0DoesNotBlackOutTheLayer) {
  DrawWithMipmapSamplerOnUnit0(/*in_frame=*/false);
}

TEST_F(LayerDrawGlEs3, SamplerLeftOnUnit0DoesNotBlackOutTheLayerInAFrame) {
  DrawWithMipmapSamplerOnUnit0(/*in_frame=*/true);
}

// A whole-image view clipped to a 2 x 2 block at (1, 1): only that block is
// drawn, and with the pixels that belong there, on either target.
TEST_F(LayerDrawGl, AClippedViewDrawsOnlyInsideItsClip) {
  std::vector<uint8_t> img(Px(0, kH, kW));
  for (int i = 0; i < kW * kH; ++i) {
    std::memcpy(&img[Px(i, 0, 0)], ColorOf(i).data(), 4);
  }
  const GLuint tex = MakeTexture(kW, kH, img);
  LayeredSurface surface;
  ICompositorSurface::GlLayerTexture whole;
  whole.name = tex;
  whole.width = kW;
  whole.height = kH;
  whole.top_first = true;
  surface.layers = {whole};
  const RectI clip{1, 1, 2, 2};

  for (const bool target_top_first : {false, true}) {
    SCOPED_TRACE(target_top_first ? "top-first target" : "window target");
    const auto out =
        DrawSurface(*compositor_, surface, target_top_first, nullptr, &clip);
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x) {
        const bool inside = x >= 1 && x < 3 && y >= 1 && y < 3;
        std::array<uint8_t, 4> want{0, 0, 0, 0};
        if (inside) {
          want = ColorOf(y * kW + x);
        }
        EXPECT_TRUE(std::memcmp(&out[Px(x, y, kW)], want.data(), 4) == 0)
            << "(" << x << ", " << y << "): got " << int(out[Px(x, y, kW)])
            << "," << int(out[Px(x, y, kW) + 1]) << ","
            << int(out[Px(x, y, kW) + 2]) << "," << int(out[Px(x, y, kW) + 3]);
      }
    }
  }
  glDeleteTextures(1, &tex);
}

namespace {

// Whether each channel of @p got is within 2 of @p want: the opacity goes
// through a mediump multiply and an 8-bit target.
bool Near(const uint8_t* got, const std::array<int, 4>& want) {
  for (int k = 0; k < 4; ++k) {
    if (std::abs(int(got[k]) - want[k]) > 2) {
      return false;
    }
  }
  return true;
}

}  // namespace

// A whole-image view at opacity 0.5 over a cleared target: every pixel is its
// premultiplied color at half, alpha included, on either target.
TEST_F(LayerDrawGl, ATranslucentViewIsDrawnAtItsOpacity) {
  std::vector<uint8_t> img(Px(0, kH, kW));
  for (int i = 0; i < kW * kH; ++i) {
    std::memcpy(&img[Px(i, 0, 0)], ColorOf(i).data(), 4);
  }
  const GLuint tex = MakeTexture(kW, kH, img);
  LayeredSurface surface;
  ICompositorSurface::GlLayerTexture whole;
  whole.name = tex;
  whole.width = kW;
  whole.height = kH;
  whole.top_first = true;
  surface.layers = {whole};

  for (const bool target_top_first : {false, true}) {
    SCOPED_TRACE(target_top_first ? "top-first target" : "window target");
    const auto out = DrawSurface(*compositor_, surface, target_top_first,
                                 nullptr, nullptr, /*alpha=*/0.5f);
    for (int i = 0; i < kW * kH; ++i) {
      const auto c = ColorOf(i);
      const std::array<int, 4> want = {c[0] / 2, c[1] / 2, c[2] / 2, 128};
      const uint8_t* got = &out[Px(i, 0, 0)];
      EXPECT_TRUE(Near(got, want))
          << "pixel " << i << ": got " << int(got[0]) << "," << int(got[1])
          << "," << int(got[2]) << "," << int(got[3]);
    }
  }
  glDeleteTextures(1, &tex);
}

// A view of two layers at opacity 0.5: a background, and above it an opaque
// 2 x 1 strip. At full opacity the strip would overwrite the background; at
// 0.5 it blends over it like any other layer, so the background shows
// through: 0.5 * strip + (1 - 0.5) * (0.5 * background).
TEST_F(LayerDrawGl, AnOpaqueLayerOfATranslucentViewBlends) {
  const std::array<uint8_t, 4> kBg = {200, 100, 0, 255};
  const std::array<uint8_t, 4> kStrip = {0, 0, 240, 255};
  std::vector<uint8_t> bg(Px(0, kH, kW));
  for (int i = 0; i < kW * kH; ++i) {
    std::memcpy(&bg[Px(i, 0, 0)], kBg.data(), 4);
  }
  std::vector<uint8_t> strip(8);
  std::memcpy(&strip[0], kStrip.data(), 4);
  std::memcpy(&strip[4], kStrip.data(), 4);
  const GLuint bg_tex = MakeTexture(kW, kH, bg);
  const GLuint strip_tex = MakeTexture(2, 1, strip);
  LayeredSurface surface;
  ICompositorSurface::GlLayerTexture base;
  base.name = bg_tex;
  base.width = kW;
  base.height = kH;
  base.top_first = true;
  ICompositorSurface::GlLayerTexture top;
  top.name = strip_tex;
  top.width = 2;
  top.height = 1;
  top.top_first = true;
  top.geometry.dst = {1, 1, 2, 1};
  top.geometry.opaque = true;
  surface.layers = {base, top};

  const auto out =
      DrawSurface(*compositor_, surface, /*target_top_first=*/false, nullptr,
                  nullptr, /*alpha=*/0.5f);
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      const bool on_strip = y == 1 && (x == 1 || x == 2);
      std::array<int, 4> want = {kBg[0] / 2, kBg[1] / 2, kBg[2] / 2, 128};
      if (on_strip) {
        want = {kStrip[0] / 2 + kBg[0] / 4, kStrip[1] / 2 + kBg[1] / 4,
                kStrip[2] / 2 + kBg[2] / 4, 191};
      }
      const uint8_t* got = &out[Px(x, y, kW)];
      EXPECT_TRUE(Near(got, want))
          << "(" << x << ", " << y << "): got " << int(got[0]) << ","
          << int(got[1]) << "," << int(got[2]) << "," << int(got[3]);
    }
  }
  glDeleteTextures(1, &bg_tex);
  glDeleteTextures(1, &strip_tex);
}

namespace {

constexpr int kRw = 20;  // the render space of the rounded-clip tests
constexpr int kRh = 12;

// Draw `surface` across a kRw x kRh view with `rounded` into a target of
// w x h, through `rotation` when it is not null, and read it back top row
// first.
std::vector<uint8_t> DrawRounded(GlCompositor& compositor,
                                 const ICompositorSurface& surface,
                                 int w,
                                 int h,
                                 bool target_top_first,
                                 const DisplayRotation* rotation,
                                 const RoundedRect& rounded) {
  GLuint target = 0;
  glGenTextures(1, &target);
  glBindTexture(GL_TEXTURE_2D, target);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               nullptr);
  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         target, 0);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  compositor.CompositeSurfaceLayers(fbo, surface, {0, 0, kRw, kRh}, h,
                                    target_top_first, /*blend_first=*/false,
                                    nullptr, nullptr, rotation, nullptr, 1.0f,
                                    &rounded);
  std::vector<uint8_t> rows(Px(0, h, w));
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(1, &target);
  if (target_top_first) {
    return rows;
  }
  std::vector<uint8_t> top_down(rows.size());
  for (int y = 0; y < h; ++y) {
    std::memcpy(&top_down[Px(0, y, w)], &rows[Px(0, h - 1 - y, w)],
                Px(w, 0, 0));
  }
  return top_down;
}

}  // namespace

// A view clipped to its own rect with only the upper left corner rounded, at
// a radius of 6: the pixels outside the curve are cut away, one on it is
// partly covered, the ones past it show, and the other three corners stay
// square. On either target,
// and turned onto a panel a quarter turn either way, where each render pixel
// is looked for where the turn puts it.
TEST_F(LayerDrawGl, ARoundedClipCutsOnlyItsCorners) {
  const std::array<uint8_t, 4> kColor = {40, 160, 220, 255};
  std::vector<uint8_t> img(Px(0, kRh, kRw));
  for (int i = 0; i < kRw * kRh; ++i) {
    std::memcpy(&img[Px(i, 0, 0)], kColor.data(), 4);
  }
  const GLuint tex = MakeTexture(kRw, kRh, img);
  LayeredSurface surface;
  ICompositorSurface::GlLayerTexture whole;
  whole.name = tex;
  whole.width = kRw;
  whole.height = kRh;
  whole.top_first = true;
  surface.layers = {whole};
  RoundedRect rounded;
  rounded.rect = {0, 0, kRw, kRh};
  rounded.radii[0] = {6, 6};

  struct Case {
    int degrees;
    bool top_first;
  };
  for (const Case c : {Case{0, false}, Case{0, true}, Case{90, false},
                       Case{270, false}, Case{90, true}}) {
    SCOPED_TRACE(::testing::Message()
                 << c.degrees << " degrees, "
                 << (c.top_first ? "top-first" : "window") << " target");
    const DisplayRotation rot{c.degrees, kRw, kRh, c.degrees % 180 ? kRh : kRw,
                              c.degrees % 180 ? kRw : kRh};
    const int pw = rot.panel_w;
    const int ph = rot.panel_h;
    const auto out = DrawRounded(*compositor_, surface, pw, ph, c.top_first,
                                 c.degrees != 0 ? &rot : nullptr, rounded);
    // The panel pixel showing render pixel (x, y).
    const auto at = [&](int x, int y) -> const uint8_t* {
      double px = x + 0.5;
      double py = y + 0.5;
      if (c.degrees != 0) {
        rot.ToPanel(x + 0.5, y + 0.5, &px, &py);
      }
      return &out[Px(static_cast<int>(px), static_cast<int>(py), pw)];
    };
    EXPECT_EQ(at(0, 0)[3], 0) << "the rounded corner is cut away";
    EXPECT_EQ(at(1, 0)[3], 0) << "and the pixel beside it, outside the curve";
    // (1, 1) is on the curve: neither cut nor whole.
    EXPECT_GT(at(1, 1)[3], 0);
    EXPECT_LT(at(1, 1)[3], 255);
    EXPECT_EQ(at(3, 3)[3], 255) << "past the curve the view shows";
    EXPECT_EQ(at(6, 0)[3], 255) << "past the corner along the top edge";
    EXPECT_EQ(at(0, 6)[3], 255) << "and down the left edge";
    for (const auto& [x, y] :
         {std::pair{kRw - 1, 0}, std::pair{kRw - 1, kRh - 1},
          std::pair{0, kRh - 1}}) {
      EXPECT_TRUE(std::memcmp(at(x, y), kColor.data(), 4) == 0)
          << "square corner (" << x << ", " << y << ")";
    }
    // Outside the corner's box the quad is drawn in pieces without the mask:
    // every pixel there is the view, with no seam between the pieces.
    int off = 0;
    for (int y = 0; y < kRh; ++y) {
      for (int x = 0; x < kRw; ++x) {
        if ((x >= 6 || y >= 6) &&
            std::memcmp(at(x, y), kColor.data(), 4) != 0) {
          ++off;
        }
      }
    }
    EXPECT_EQ(off, 0) << "pixels outside the corner box that are not the view";
  }
  glDeleteTextures(1, &tex);
}

namespace {

// Draw `surface`, a view of view_w x view_h in its own pixels, through `map`
// into a target of w x h, clipped to `clip` when not null, through `rotation`
// when not null, and read it back top row first.
std::vector<uint8_t> DrawThrough(GlCompositor& compositor,
                                 const ICompositorSurface& surface,
                                 int view_w,
                                 int view_h,
                                 const ViewMap& map,
                                 int w,
                                 int h,
                                 bool target_top_first,
                                 const DisplayRotation* rotation,
                                 const RectI* clip,
                                 const RoundedRect* rounded = nullptr) {
  GLuint target = 0;
  glGenTextures(1, &target);
  glBindTexture(GL_TEXTURE_2D, target);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               nullptr);
  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         target, 0);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  compositor.CompositeSurfaceLayers(fbo, surface, {0, 0, view_w, view_h}, h,
                                    target_top_first,
                                    /*blend_first=*/false, nullptr, nullptr,
                                    rotation, clip, 1.0f, rounded, &map);
  std::vector<uint8_t> rows(Px(0, h, w));
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(1, &target);
  if (target_top_first) {
    return rows;
  }
  std::vector<uint8_t> top_down(rows.size());
  for (int y = 0; y < h; ++y) {
    std::memcpy(&top_down[Px(0, h - 1 - y, w)], &rows[Px(0, y, w)],
                Px(w, 0, 0));
  }
  return top_down;
}

// A view whose every pixel has its own color: red from x, green from y.
std::vector<uint8_t> Ramp(int w, int h) {
  std::vector<uint8_t> img(Px(0, h, w));
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t* p = &img[Px(x, y, w)];
      p[0] = static_cast<uint8_t>(10 * x + 5);
      p[1] = static_cast<uint8_t>(20 * y + 5);
      p[2] = 99;
      p[3] = 255;
    }
  }
  return img;
}

}  // namespace

// A view turned a quarter turn, (x, y) -> (kRh - y, x): every output pixel is
// the view's pixel the turn puts there. On either target, and on a display
// itself turned a quarter turn either way.
TEST_F(LayerDrawGl, ARotatedViewIsDrawnTurned) {
  const std::vector<uint8_t> img = Ramp(kRw, kRh);
  const GLuint tex = MakeTexture(kRw, kRh, img);
  LayeredSurface surface;
  ICompositorSurface::GlLayerTexture whole;
  whole.name = tex;
  whole.width = kRw;
  whole.height = kRh;
  whole.top_first = true;
  surface.layers = {whole};
  ViewMap map;
  map.a = 0;
  map.b = 1;
  map.c = -1;
  map.d = 0;
  map.tx = kRh;
  map.ty = 0;
  // The turned view fills a render space kRh wide and kRw tall.
  constexpr int kOw = kRh;
  constexpr int kOh = kRw;

  struct Case {
    int degrees;
    bool top_first;
  };
  for (const Case c : {Case{0, false}, Case{0, true}, Case{90, false},
                       Case{270, false}, Case{90, true}}) {
    SCOPED_TRACE(::testing::Message()
                 << c.degrees << " degrees, "
                 << (c.top_first ? "top-first" : "window") << " target");
    const DisplayRotation rot{c.degrees, kOw, kOh, c.degrees % 180 ? kOh : kOw,
                              c.degrees % 180 ? kOw : kOh};
    const int pw = rot.panel_w;
    const int ph = rot.panel_h;
    const auto out =
        DrawThrough(*compositor_, surface, kRw, kRh, map, pw, ph, c.top_first,
                    c.degrees != 0 ? &rot : nullptr, nullptr);
    int wrong = 0;
    for (int ry = 0; ry < kOh; ++ry) {
      for (int rx = 0; rx < kOw; ++rx) {
        double px = rx + 0.5;
        double py = ry + 0.5;
        if (c.degrees != 0) {
          rot.ToPanel(rx + 0.5, ry + 0.5, &px, &py);
        }
        const uint8_t* got =
            &out[Px(static_cast<int>(px), static_cast<int>(py), pw)];
        // Render pixel (rx, ry) is the view's pixel (ry, kRh - 1 - rx).
        const uint8_t* want = &img[Px(ry, kRh - 1 - rx, kRw)];
        if (std::memcmp(got, want, 4) != 0) {
          if (wrong++ < 3) {
            ADD_FAILURE() << "render (" << rx << ", " << ry << ") got "
                          << int{got[0]} << "," << int{got[1]} << " want "
                          << int{want[0]} << "," << int{want[1]};
          }
        }
      }
    }
    EXPECT_EQ(wrong, 0);
  }
  glDeleteTextures(1, &tex);
}

// An eighth of a turn about the target's center: the view is a diamond, and
// the corners of its bounding box stay as they were.
TEST_F(LayerDrawGl, AnEighthTurnLeavesTheBoundsCornersAlone) {
  constexpr int kV = 8;
  const std::vector<uint8_t> img = Ramp(kV, kV);
  const GLuint tex = MakeTexture(kV, kV, img);
  LayeredSurface surface;
  ICompositorSurface::GlLayerTexture whole;
  whole.name = tex;
  whole.width = kV;
  whole.height = kV;
  whole.top_first = true;
  surface.layers = {whole};
  // About the view's center (4, 4), to the target's center (8, 8).
  const double k = std::sqrt(0.5);
  ViewMap map;
  map.a = k;
  map.b = k;
  map.c = -k;
  map.d = k;
  map.tx = 8 - (k * 4 - k * 4);
  map.ty = 8 - (k * 4 + k * 4);
  const auto out = DrawThrough(*compositor_, surface, kV, kV, map, 16, 16,
                               false, nullptr, nullptr);
  // The diamond's corners are 4 * sqrt(2) from (8, 8) along the axes.
  EXPECT_EQ(out[Px(8, 8, 16) + 3], 255) << "the center";
  EXPECT_EQ(out[Px(8, 3, 16) + 3], 255) << "near the top corner";
  EXPECT_EQ(out[Px(3, 3, 16) + 3], 0) << "the bounds' upper left";
  EXPECT_EQ(out[Px(12, 3, 16) + 3], 0) << "the bounds' upper right";
  EXPECT_EQ(out[Px(3, 12, 16) + 3], 0) << "the bounds' lower left";
  EXPECT_EQ(out[Px(12, 12, 16) + 3], 0) << "the bounds' lower right";
  glDeleteTextures(1, &tex);
}

// The turned view under a clip: rows past it are not drawn, and the rows
// above it are the turned view.
TEST_F(LayerDrawGl, ARotatedViewIsClipped) {
  const std::vector<uint8_t> img = Ramp(kRw, kRh);
  const GLuint tex = MakeTexture(kRw, kRh, img);
  LayeredSurface surface;
  ICompositorSurface::GlLayerTexture whole;
  whole.name = tex;
  whole.width = kRw;
  whole.height = kRh;
  whole.top_first = true;
  surface.layers = {whole};
  ViewMap map;
  map.a = 0;
  map.b = 1;
  map.c = -1;
  map.d = 0;
  map.tx = kRh;
  const RectI clip{0, 0, kRh, 10};
  const auto out = DrawThrough(*compositor_, surface, kRw, kRh, map, kRh, kRw,
                               false, nullptr, &clip);
  int drawn_past = 0;
  int missing = 0;
  for (int y = 0; y < kRw; ++y) {
    for (int x = 0; x < kRh; ++x) {
      const uint8_t* got = &out[Px(x, y, kRh)];
      drawn_past += y >= 10 && got[3] != 0 ? 1 : 0;
      // Above the clip, the turned view's pixel (y, kRh - 1 - x).
      missing +=
          y < 10 && std::memcmp(got, &img[Px(y, kRh - 1 - x, kRw)], 4) != 0 ? 1
                                                                            : 0;
    }
  }
  EXPECT_EQ(drawn_past, 0);
  EXPECT_EQ(missing, 0);
  glDeleteTextures(1, &tex);
}

// A view under a perspective, w = 1 + x / 20: its right edge is drawn at half
// its distance, a trapezoid. Each output pixel inside it samples the view's
// pixel the perspective puts there, which interpolating the texture
// coordinates across the screen would not: the draw has to divide by w per
// pixel. On either target, and on a display turned a quarter turn.
TEST_F(LayerDrawGl, APerspectiveViewIsSampledPerspectiveCorrect) {
  const std::vector<uint8_t> img = Ramp(kRw, kRh);
  const GLuint tex = MakeTexture(kRw, kRh, img);
  LayeredSurface surface;
  ICompositorSurface::GlLayerTexture whole;
  whole.name = tex;
  whole.width = kRw;
  whole.height = kRh;
  whole.top_first = true;
  surface.layers = {whole};
  ViewMap map;
  map.g = 1.0 / kRw;
  // The trapezoid fits a render space kRw / 2 wide and kRh tall.
  constexpr int kOw = kRw / 2;
  constexpr int kOh = kRh;

  struct Case {
    int degrees;
    bool top_first;
  };
  for (const Case c : {Case{0, false}, Case{0, true}, Case{90, false}}) {
    SCOPED_TRACE(::testing::Message()
                 << c.degrees << " degrees, "
                 << (c.top_first ? "top-first" : "window") << " target");
    const DisplayRotation rot{c.degrees, kOw, kOh, c.degrees % 180 ? kOh : kOw,
                              c.degrees % 180 ? kOw : kOh};
    const int pw = rot.panel_w;
    const int ph = rot.panel_h;
    const auto out =
        DrawThrough(*compositor_, surface, kRw, kRh, map, pw, ph, c.top_first,
                    c.degrees != 0 ? &rot : nullptr, nullptr);
    int checked = 0;
    int wrong = 0;
    for (int ry = 0; ry < kOh; ++ry) {
      for (int rx = 0; rx < kOw; ++rx) {
        // The view's point under render pixel (rx, ry)'s center: x / w = px
        // with w = 1 + x / kRw.
        const double px = rx + 0.5;
        const double py = ry + 0.5;
        const double w = 1.0 / (1.0 - px / kRw);
        const double x = px * w;
        const double y = py * w;
        double qx = px;
        double qy = py;
        if (c.degrees != 0) {
          rot.ToPanel(px, py, &qx, &qy);
        }
        const uint8_t* got =
            &out[Px(static_cast<int>(qx), static_cast<int>(qy), pw)];
        // Away from the edges and from texel boundaries, where rounding
        // could pick either side.
        const bool inside =
            x > 0.6 && x < kRw - 0.6 && y > 0.6 && y < kRh - 0.6;
        const bool outside = y > kRh + 1.0;
        const double fx = x - std::floor(x);
        const double fy = y - std::floor(y);
        if (outside) {
          wrong += got[3] != 0 ? 1 : 0;
          continue;
        }
        if (!inside || fx < 0.05 || fx > 0.95 || fy < 0.05 || fy > 0.95) {
          continue;
        }
        ++checked;
        const uint8_t* want =
            &img[Px(static_cast<int>(x), static_cast<int>(y), kRw)];
        if (std::memcmp(got, want, 4) != 0) {
          if (wrong++ < 3) {
            ADD_FAILURE() << "render (" << rx << ", " << ry << ") got "
                          << int{got[0]} << "," << int{got[1]} << " want "
                          << int{want[0]} << "," << int{want[1]};
          }
        }
      }
    }
    EXPECT_GT(checked, 30);
    EXPECT_EQ(wrong, 0);
  }
  glDeleteTextures(1, &tex);
}

// The same perspective view under a rounded clip in output pixels: the clip's
// corner is cut where the clip is, not where an affine guess at the pixel's
// position would put it.
TEST_F(LayerDrawGl, ARoundedClipCutsAPerspectiveView) {
  const std::vector<uint8_t> img = Ramp(kRw, kRh);
  const GLuint tex = MakeTexture(kRw, kRh, img);
  LayeredSurface surface;
  ICompositorSurface::GlLayerTexture whole;
  whole.name = tex;
  whole.width = kRw;
  whole.height = kRh;
  whole.top_first = true;
  surface.layers = {whole};
  ViewMap map;
  map.g = 1.0 / kRw;
  constexpr int kOw = kRw / 2;
  constexpr int kOh = kRh;
  // Its upper right corner, at the trapezoid's narrow side, where w is
  // largest: (kOw, 0).
  RoundedRect rounded;
  rounded.rect = {0, 0, kOw, kOh};
  rounded.radii[1] = {4, 4};
  const auto out = DrawThrough(*compositor_, surface, kRw, kRh, map, kOw, kOh,
                               false, nullptr, nullptr, &rounded);
  EXPECT_EQ(out[Px(kOw - 1, 0, kOw) + 3], 0) << "the corner is cut";
  EXPECT_EQ(out[Px(kOw - 2, 2, kOw) + 3], 255) << "inside the curve";
  EXPECT_EQ(out[Px(0, 0, kOw) + 3], 255) << "the other corner is square";
  // On the curve, partly covered. At these pixels w is 1.6 to 1.9, and the
  // point not divided by it would be well outside the curve and fully cut.
  for (const auto& [x, y] : {std::pair{kOw - 1, 1}, std::pair{kOw - 3, 0}}) {
    const uint8_t a = out[Px(x, y, kOw) + 3];
    EXPECT_GT(a, 0) << x << "," << y;
    EXPECT_LT(a, 255) << x << "," << y;
  }
  glDeleteTextures(1, &tex);
}
