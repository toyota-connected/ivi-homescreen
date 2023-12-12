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

#include "engine.h"
#include "logging.h"

Texture::Texture(const uint32_t id,
                 const uint32_t target,
                 const uint32_t format,
                 const EncodableValueCallback create_callback,
                 const TextureCallback dispose_callback,
                 const int width,
                 const int height)
    : m_flutter_engine(nullptr),
      m_enabled(false),
      m_id(id),
      m_name({}),
      m_target(target),
      m_format(format),
      m_width(width),
      m_height(height),
      m_draw_next(false),
      m_create_callback(create_callback),
      m_dispose_callback(dispose_callback) {}

Texture::~Texture() {
  SPDLOG_DEBUG("Texture Destructor");
}

void Texture::GetFlutterOpenGLTexture(FlutterOpenGLTexture* texture_out) {
  texture_out->target = m_target;
  texture_out->format = m_format;
  m_draw_next = true;
}

flutter::EncodableValue Texture::Create(
    int32_t width,
    int32_t height,
    const std::map<flutter::EncodableValue, flutter::EncodableValue>* args) {
  m_width = width;
  m_height = height;
  if (m_create_callback) {
    return m_create_callback(this, args);
  }

  return flutter::EncodableValue(flutter::EncodableMap{
      {flutter::EncodableValue("result"), flutter::EncodableValue(-1)},
      {flutter::EncodableValue("error"),
       flutter::EncodableValue("Create callback not set")}});
}

void Texture::Dispose(GLuint name) {
  if (m_dispose_callback) {
    m_dispose_callback(this, name);
  }
}

void Texture::Enable(GLuint name) {
  if (m_flutter_engine) {
    m_name.push_back(static_cast<int64_t>(name));

    // Add again for assigned EGL texture id
    // EGL assigns Textures starting in low digits
    // Keep values passed to open_gl texture high to prevent collision
    // no plan to enforce overwriting
    m_flutter_engine->TextureRegistryAdd(static_cast<int64_t>(name), this);

    if (kSuccess !=
        m_flutter_engine->TextureEnable(static_cast<int64_t>(name))) {
      assert(false);
    }

    if (kSuccess != Engine::MarkExternalTextureFrameAvailable(
                        m_flutter_engine, static_cast<int64_t>(name))) {
      assert(false);
    }
    m_enabled = true;
  }
}

void Texture::Disable(GLuint name) {
  assert(m_flutter_engine);
  assert(name);

  m_flutter_engine->TextureDisable(static_cast<int64_t>(name));
  m_enabled = false;

  const auto i = find(m_name.begin(), m_name.end(), name);
  if (i != m_name.end()) {
    m_name.erase(i);
  }
}

void Texture::SetEngine(Engine* engine) {
  if (engine) {
    m_flutter_engine = engine;
    engine->TextureRegistryAdd(m_id, this);
  }
}

void Texture::FrameReady() const {
  if (m_flutter_engine)
    for (const auto name : m_name) {
      Engine::MarkExternalTextureFrameAvailable(m_flutter_engine, name);
    }
}
