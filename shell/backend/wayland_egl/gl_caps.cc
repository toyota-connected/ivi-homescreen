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

#include "backend/wayland_egl/gl_caps.h"

#include <cstring>
#include <string_view>

#include "backend/gl_process_resolver.h"
#include "logging.h"

namespace {

bool ExtensionSupported(const char* extensions, const char* name) {
  if (!extensions || !name) {
    return false;
  }
  const size_t name_len = std::strlen(name);
  std::string_view sv(extensions);
  size_t pos = 0;
  while ((pos = sv.find(name, pos)) != std::string_view::npos) {
    const bool left_ok = (pos == 0) || (sv[pos - 1] == ' ');
    const size_t end = pos + name_len;
    const bool right_ok = (end == sv.size()) || (sv[end] == ' ');
    if (left_ok && right_ok) {
      return true;
    }
    pos = end;
  }
  return false;
}

void* Resolve(const char* name) {
  return GlProcessResolver::GetInstance().process_resolver(name);
}

}  // namespace

void GlCaps::Probe() {
  const char* version =
      reinterpret_cast<const char*>(glGetString(GL_VERSION));
  if (version) {
    // "OpenGL ES N.M ..." — skip leading prefix if present.
    const char* p = version;
    if (const char* space = std::strchr(p, ' ')) {
      // Advance past "OpenGL ES" (two words) to the version digits.
      if (std::strncmp(p, "OpenGL", 6) == 0) {
        p = space + 1;
        if (const char* space2 = std::strchr(p, ' ')) {
          p = space2 + 1;
        }
      }
    }
    context_major = 0;
    context_minor = 0;
    // Parse "N.M" leniently.
    while (*p >= '0' && *p <= '9') {
      context_major = context_major * 10 + (*p - '0');
      ++p;
    }
    if (*p == '.') {
      ++p;
      while (*p >= '0' && *p <= '9') {
        context_minor = context_minor * 10 + (*p - '0');
        ++p;
      }
    }
  }
  is_es3 = (context_major >= 3);

  const char* exts =
      reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));

  has_rgb8_rgba8 =
      is_es3 || ExtensionSupported(exts, "GL_OES_rgb8_rgba8");
  has_packed_depth_stencil =
      is_es3 || ExtensionSupported(exts, "GL_OES_packed_depth_stencil");

  // Resolve blit_framebuffer.
  blit_framebuffer = nullptr;
  if (is_es3) {
    blit_framebuffer = reinterpret_cast<BlitFramebufferFn>(
        Resolve("glBlitFramebuffer"));
  }
  if (!blit_framebuffer && ExtensionSupported(exts, "GL_NV_framebuffer_blit")) {
    blit_framebuffer = reinterpret_cast<BlitFramebufferFn>(
        Resolve("glBlitFramebufferNV"));
  }
  if (!blit_framebuffer &&
      ExtensionSupported(exts, "GL_ANGLE_framebuffer_blit")) {
    blit_framebuffer = reinterpret_cast<BlitFramebufferFn>(
        Resolve("glBlitFramebufferANGLE"));
  }
  has_blit_framebuffer = (blit_framebuffer != nullptr);

  // Resolve renderbuffer_storage_multisample.
  renderbuffer_storage_multisample = nullptr;
  if (is_es3) {
    renderbuffer_storage_multisample =
        reinterpret_cast<RenderbufferStorageMultisampleFn>(
            Resolve("glRenderbufferStorageMultisample"));
  }
  if (!renderbuffer_storage_multisample &&
      ExtensionSupported(exts, "GL_ANGLE_framebuffer_multisample")) {
    renderbuffer_storage_multisample =
        reinterpret_cast<RenderbufferStorageMultisampleFn>(
            Resolve("glRenderbufferStorageMultisampleANGLE"));
  }
  if (!renderbuffer_storage_multisample &&
      ExtensionSupported(exts, "GL_NV_framebuffer_multisample")) {
    renderbuffer_storage_multisample =
        reinterpret_cast<RenderbufferStorageMultisampleFn>(
            Resolve("glRenderbufferStorageMultisampleNV"));
  }
  has_multisampled_renderbuffer =
      (renderbuffer_storage_multisample != nullptr);

  spdlog::debug(
      "GlCaps: ES{}.{} rgb8={} packed_ds={} blit={} msaa_rb={}",
      context_major, context_minor, has_rgb8_rgba8, has_packed_depth_stencil,
      has_blit_framebuffer, has_multisampled_renderbuffer);
}
