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

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

/**
 * @brief Runtime GL capabilities probed from the current context.
 *
 * Compositor mode must work against both GLES2 and GLES3 contexts. The
 * features it depends on (sized internal formats, packed depth/stencil,
 * framebuffer blit, multisample renderbuffers) are ES3 core but available
 * on ES2 via a mix of extensions. This struct caches both the feature
 * flags and the resolved function pointers so the hot path never re-probes.
 */
struct GlCaps {
  // Context version parsed from glGetString(GL_VERSION).
  int context_major{0};
  int context_minor{0};
  bool is_es3{false};

  // Feature flags — true if either ES3 core or the matching extension.
  bool has_rgb8_rgba8{false};
  bool has_packed_depth_stencil{false};
  bool has_blit_framebuffer{false};
  bool has_multisampled_renderbuffer{false};

  // Resolved entry points. Signatures match ES3 core; the NV / ANGLE
  // extensions are ABI-compatible, so the same pointer type is used.
  using BlitFramebufferFn = void(GL_APIENTRY*)(GLint srcX0,
                                               GLint srcY0,
                                               GLint srcX1,
                                               GLint srcY1,
                                               GLint dstX0,
                                               GLint dstY0,
                                               GLint dstX1,
                                               GLint dstY1,
                                               GLbitfield mask,
                                               GLenum filter);

  using RenderbufferStorageMultisampleFn =
      void(GL_APIENTRY*)(GLenum target,
                         GLsizei samples,
                         GLenum internalformat,
                         GLsizei width,
                         GLsizei height);

  BlitFramebufferFn blit_framebuffer{nullptr};
  RenderbufferStorageMultisampleFn renderbuffer_storage_multisample{nullptr};

  /**
   * @brief Probe the current GL context. Safe to call more than once.
   *
   * Must be invoked with a current context; uses @c glGetString and the
   * engine's GL proc resolver to look up entry points.
   */
  void Probe();
};
