// Copyright 2020 Toyota Connected North America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "texture.h"

#include <cassert>

#include <flutter/fml/logging.h>

#include "engine.h"

Texture::Texture(uint32_t id,
                 uint32_t target,
                 uint32_t format,
                 VoidCallback create_callback,
                 VoidCallback dispose_callback,
                 int width,
                 int height)
    : m_flutter_engine(nullptr),
      m_create_callback(create_callback),
      m_dispose_callback(dispose_callback),
      m_enabled(false),
      m_draw_next(false),
      m_target(target),
      m_id(id),
      m_name(0),
      m_format(format),
      m_height(height),
      m_width(width) {}

Texture::~Texture() {
  FML_DLOG(INFO) << "Texture Destructor";
}

void Texture::GetFlutterOpenGLTexture(FlutterOpenGLTexture* texture_out,
                                      int width,
                                      int height) {
  texture_out->width = width;
  texture_out->height = height;
  texture_out->target = m_target;
  texture_out->name = m_name;
  texture_out->format = m_format;

  m_draw_next = true;
}

int64_t Texture::Create(int32_t width, int32_t height) {
  m_width = width;
  m_height = height;
  if (m_create_callback) {
    m_create_callback(this);
  }
  return m_name;
}

void Texture::Dispose() {
  if (m_dispose_callback) {
    m_dispose_callback(this);
  }
}

void Texture::Enable(GLuint name) {
  m_name = name;

  if (m_flutter_engine) {
    // Add again for assigned EGL texture id
    // EGL assigns Textures starting in low digits
    // Keep values passed to open_gl texture high to prevent collision
    // no plan to enforce overwriting
    m_flutter_engine->TextureRegistryAdd(m_name, this);

    if (kSuccess != m_flutter_engine->TextureEnable(m_name)) {
      assert(false);
    }

    if (kSuccess != m_flutter_engine->MarkExternalTextureFrameAvailable(
                        m_flutter_engine, m_name)) {
      assert(false);
    }
    m_enabled = true;
  }
}

void Texture::Disable() {
  if (m_flutter_engine)
    assert(m_name);
  m_flutter_engine->TextureDisable(m_name);
  m_enabled = false;
}

void Texture::SetEngine(const std::shared_ptr<Engine>& engine) {
  if (engine) {
    m_flutter_engine = engine;
    engine->TextureRegistryAdd(m_id, this);
  }
}

void Texture::FrameReady() {
  if (m_flutter_engine)
    m_flutter_engine->MarkExternalTextureFrameAvailable(m_flutter_engine,
                                                        m_name);
}
