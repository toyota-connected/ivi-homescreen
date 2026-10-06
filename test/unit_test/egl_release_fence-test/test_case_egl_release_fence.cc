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

// EglDmabufImporter::CreateReleaseFenceFd on a real EGL display: it hands back
// a sync_file, which is what a producer on an explicit-sync grant can wait on
// in the GPU. Before #720 the only release signal the EGL paths offered was an
// eventfd, which a producer has to poll on the CPU -- so "it is an fd" is not
// the assertion that matters; "it is a sync_file" is.
//
// Skips itself without Mesa's surfaceless platform, without a current context,
// or on a display that cannot export a native fence.

#include <unistd.h>

#include <array>
#include <cstring>
#include <string>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <gtest/gtest.h>

#include "egl_dmabuf_import.h"

namespace {

// What the kernel calls the anon inode behind a sync_file. An eventfd reads
// back as "anon_inode:[eventfd]", which is exactly the difference under test.
std::string FdKind(int fd) {
  std::array<char, 128> buf{};
  const std::string link = "/proc/self/fd/" + std::to_string(fd);
  const ssize_t n = ::readlink(link.c_str(), buf.data(), buf.size() - 1);
  if (n < 0) {
    return {};
  }
  return std::string(buf.data(), static_cast<size_t>(n));
}

class EglReleaseFence : public ::testing::Test {
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
        std::strstr(exts, "EGL_KHR_surfaceless_context") == nullptr) {
      GTEST_SKIP() << "no EGL_KHR_surfaceless_context";
    }
    if (!importer_.Init(display_)) {
      GTEST_SKIP() << "no EGL_EXT_image_dma_buf_import";
    }

    // A current context, because the fence is made from its command stream.
    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
      GTEST_SKIP() << "no GLES API";
    }
    const EGLint cfg_attribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                  EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                  EGL_NONE};
    EGLConfig config = nullptr;
    EGLint n = 0;
    if (eglChooseConfig(display_, cfg_attribs, &config, 1, &n) != EGL_TRUE ||
        n < 1) {
      GTEST_SKIP() << "no renderable EGL config";
    }
    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, ctx_attribs);
    if (context_ == EGL_NO_CONTEXT ||
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_) !=
            EGL_TRUE) {
      GTEST_SKIP() << "could not make a surfaceless context current";
    }
  }

  void TearDown() override {
    if (display_ != EGL_NO_DISPLAY) {
      eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (context_ != EGL_NO_CONTEXT) {
        eglDestroyContext(display_, context_);
      }
      eglTerminate(display_);
    }
  }

  EGLDisplay display_{EGL_NO_DISPLAY};
  EGLContext context_{EGL_NO_CONTEXT};
  EglDmabufImporter importer_;
};

// The point of the change: a GPU-waitable sync_file, not an eventfd.
TEST_F(EglReleaseFence, ExportsASyncFileNotAnEventfd) {
  if (!importer_.has_release_fence()) {
    GTEST_SKIP() << "display cannot export a native fence";
  }
  const int fd = importer_.CreateReleaseFenceFd();
  ASSERT_GE(fd, 0);
  const std::string kind = FdKind(fd);
  ::close(fd);
  EXPECT_NE(kind.find("sync_file"), std::string::npos)
      << "release fence is " << kind << ", not a sync_file -- a producer would "
      << "have to wait on it from the CPU";
}

// Each call owns its own fd. A shared or recycled one would be closed out from
// under a producer still waiting on it.
TEST_F(EglReleaseFence, EachFenceIsItsOwnFd) {
  if (!importer_.has_release_fence()) {
    GTEST_SKIP() << "display cannot export a native fence";
  }
  const int a = importer_.CreateReleaseFenceFd();
  const int b = importer_.CreateReleaseFenceFd();
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  EXPECT_NE(a, b);
  ::close(a);
  ::close(b);
}

// Capability-gated, not assumed: a display without the export entry point
// returns -1 rather than a bad fd, which is what leaves the producer on the
// eventfd instead of handing it something it cannot wait on.
TEST_F(EglReleaseFence, ReportsNoFenceRatherThanABadOne) {
  if (importer_.has_release_fence()) {
    GTEST_SKIP() << "this display does export fences";
  }
  EXPECT_EQ(importer_.CreateReleaseFenceFd(), -1);
}

}  // namespace
