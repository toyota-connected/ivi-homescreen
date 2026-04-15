// Copyright 2026 Toyota Connected North America
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter_desktop_texture_registrar.h"

#include <GLES2/gl2.h>

#include <limits>

#if !defined(GL_RGBA8)
#define GL_RGBA8 0x8058
#endif

bool PopulateExternalGlTextureFrame(
    FlutterDesktopTextureRegistrar* texture_registrar,
    int64_t texture_id,
    size_t width,
    size_t height,
    FlutterOpenGLTexture* texture_out) {
  // Hold the registrar mutex for the whole call: it protects the registry
  // map against concurrent Register/Unregister, and keeps |desc| alive while
  // we mutate its cached GL state. The plugin pixel-buffer callback runs
  // under the lock, so plugins must not re-enter the registrar from inside
  // it.
  std::scoped_lock<std::mutex> lock(texture_registrar->texture_mutex);
  auto& registry = texture_registrar->texture_registry;
  const auto it = registry.find(texture_id);
  if (it == registry.end() || !it->second) {
    return false;
  }
  auto& desc = it->second;

  // GPU-surface path: the plugin owns the GL texture.
  if (!desc->pixel_buffer_callback) {
    texture_out->target = desc->target;
    texture_out->name = desc->name;
    texture_out->format = desc->format;
    texture_out->user_data = desc->release_context;
    texture_out->destruction_callback = desc->release_callback;
    texture_out->width = desc->width;
    texture_out->height = desc->height;
    desc->visible_width = width;
    desc->visible_height = height;
    return true;
  }

  // Pixel-buffer path: invoke the plugin callback, upload into an
  // embedder-owned GL texture that is allocated lazily on first frame.
  const FlutterDesktopPixelBuffer* pb =
      desc->pixel_buffer_callback(width, height, desc->pixel_buffer_user_data);
  if (!pb || !pb->buffer || pb->width == 0 || pb->height == 0) {
    return false;
  }
  // Reject sizes that would not round-trip through GLsizei/uint32_t. A
  // plugin supplying an over-sized dimension is either buggy or hostile;
  // truncating would desync |desc|'s cached dims from the actual upload.
  constexpr auto kMaxDim =
      static_cast<size_t>(std::numeric_limits<GLsizei>::max());
  if (pb->width > kMaxDim || pb->height > kMaxDim) {
    return false;
  }

  const bool first_frame = desc->name == 0;
  if (first_frame) {
    GLuint name = 0;
    glGenTextures(1, &name);
    if (name == 0) {
      return false;
    }
    glBindTexture(GL_TEXTURE_2D, name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    desc->name = name;
    desc->target = GL_TEXTURE_2D;
    desc->format = GL_RGBA8;
    desc->width = 0;
    desc->height = 0;
  } else {
    glBindTexture(GL_TEXTURE_2D, desc->name);
  }

  const auto pb_w = static_cast<GLsizei>(pb->width);
  const auto pb_h = static_cast<GLsizei>(pb->height);
  if (static_cast<uint32_t>(pb->width) != desc->width ||
      static_cast<uint32_t>(pb->height) != desc->height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pb_w, pb_h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, pb->buffer);
    desc->width = static_cast<uint32_t>(pb->width);
    desc->height = static_cast<uint32_t>(pb->height);
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pb_w, pb_h, GL_RGBA,
                    GL_UNSIGNED_BYTE, pb->buffer);
  }

  desc->visible_width = width;
  desc->visible_height = height;

  texture_out->target = desc->target;
  texture_out->name = desc->name;
  texture_out->format = desc->format;
  texture_out->user_data = nullptr;
  texture_out->destruction_callback = nullptr;
  texture_out->width = desc->width;
  texture_out->height = desc->height;

  if (pb->release_callback) {
    pb->release_callback(pb->release_context);
  }
  return true;
}
