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

#include "backend/wayland_egl/gl_compositor.h"
#include "gtest/gtest.h"

namespace {

// A caps set that can blit: ES3, entry point resolved, single-sampled target.
GlCaps Blittable() {
  GlCaps caps;
  caps.is_es3 = true;
  caps.has_blit_framebuffer = true;
  caps.blit_framebuffer = reinterpret_cast<GlCaps::BlitFramebufferFn>(
      reinterpret_cast<void*>(&glViewport));
  caps.default_fb_samples = 0;
  return caps;
}

TEST(GlCompositorCanBlit, PlainCopyToDefaultBlits) {
  EXPECT_TRUE(GlCompositor::CanBlit(Blittable(), false, false, 1, 0));
}

// The defect: BUILD_EGL_ENABLE_MULTISAMPLE gives framebuffer 0 samples, and a
// blit into a multisampled draw buffer is GL_INVALID_OPERATION, so the frame
// is never composited and the screen stays black.
TEST(GlCompositorCanBlit, MultisampledDefaultTargetDoesNotBlit) {
  GlCaps caps = Blittable();
  caps.default_fb_samples = 4;
  EXPECT_FALSE(GlCompositor::CanBlit(caps, false, false, 1, 0));
}

// Our own FBOs are single-sampled, so the sample count of framebuffer 0 says
// nothing about them.
TEST(GlCompositorCanBlit, MultisampledDefaultStillBlitsToOwnFbo) {
  GlCaps caps = Blittable();
  caps.default_fb_samples = 4;
  EXPECT_TRUE(GlCompositor::CanBlit(caps, false, false, 1, 7));
}

TEST(GlCompositorCanBlit, BlendExternalAndMissingSourceTakeQuadPath) {
  EXPECT_FALSE(GlCompositor::CanBlit(Blittable(), false, true, 1, 0));
  EXPECT_FALSE(GlCompositor::CanBlit(Blittable(), true, false, 1, 0));
  EXPECT_FALSE(GlCompositor::CanBlit(Blittable(), false, false, 0, 0));
}

TEST(GlCompositorCanBlit, UnresolvedEntryPointTakesQuadPath) {
  GlCaps caps = Blittable();
  caps.blit_framebuffer = nullptr;
  EXPECT_FALSE(GlCompositor::CanBlit(caps, false, false, 1, 0));
  caps = Blittable();
  caps.has_blit_framebuffer = false;
  EXPECT_FALSE(GlCompositor::CanBlit(caps, false, false, 1, 0));
}

}  // namespace
