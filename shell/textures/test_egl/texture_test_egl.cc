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

#include "texture_test_egl.h"

#include <chrono>

#include "app.h"
#include "engine.h"
#include "textures/texture.h"
#include "wayland/window.h"

constexpr int64_t kTestTextureObjectId = 5150;

TextureTestEgl::TextureTestEgl(App* app)
    : Texture(kTestTextureObjectId, GL_TEXTURE_2D, GL_RGBA8, Create, Dispose),
      m_egl_backend(reinterpret_cast<WaylandEglBackend*>(app->GetBackend())),
      m_initialized(false) {}

TextureTestEgl::~TextureTestEgl() = default;

void TextureTestEgl::Create(void* userdata) {
  auto* obj = reinterpret_cast<TextureTestEgl*>(userdata);

  obj->m_egl_backend->MakeTextureCurrent();

  GLubyte pixels[4 * 3] = {
      255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 0,
  };

  GLuint textureId;
  glGenTextures(1, &textureId);
  glBindTexture(GL_TEXTURE_2D, textureId);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE,
               pixels);

  glFinish();
  obj->m_egl_backend->ClearCurrent();

  obj->m_initialized = true;
  obj->Enable(textureId);
}

void TextureTestEgl::Dispose(void* userdata) {
  auto* obj = (TextureTestEgl*)userdata;
  obj->Disable();
}

void TextureTestEgl::Draw(void* userdata) {
  auto* obj = (TextureTestEgl*)userdata;

  if (!m_draw_next)
    return;

  m_draw_next = false;

  obj->FrameReady();
}
