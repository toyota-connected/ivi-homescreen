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


#include "texture_test.h"

#include <chrono>

#include "app.h"
#include "egl_window.h"
#include "engine.h"
#include "platform_channel.h"
#include "textures/texture.h"

constexpr int64_t kTestTextureObjectId = 5150;

TextureTest::TextureTest(App* app)
    : Texture(kTestTextureObjectId, GL_TEXTURE_2D, GL_RGBA8, Create, Dispose),
      m_egl_window(app->GetEglWindow(0)),
      m_initialized(false) {}

TextureTest::~TextureTest() = default;

void TextureTest::Create(void* userdata) {
  auto* obj = (TextureTest*)userdata;

  obj->m_egl_window->MakeTextureCurrent();

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
  obj->m_egl_window->ClearCurrent();

  obj->m_initialized = true;
  obj->Enable(textureId);
}

void TextureTest::Dispose(void* userdata) {
  auto* obj = (TextureTest*)userdata;
  obj->Disable();
}

void TextureTest::Draw(void* userdata) {
  auto* obj = (TextureTest*)userdata;

  if (!m_draw_next)
    return;

  m_draw_next = false;

  obj->FrameReady();
}
