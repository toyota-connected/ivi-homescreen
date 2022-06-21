/*
 * Copyright 2020 Toyota Connected North America
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

#include <memory>
#include <string>

#include <GLES2/gl2.h>
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

#include <flutter_embedder.h>

#include "backend/wayland_egl.h"
#include "flutter/fml/macros.h"
#include "textures/texture.h"

class App;
class Engine;

class TextureTestEgl : public Texture {
 public:
  explicit TextureTestEgl(App* app);
  ~TextureTestEgl();

  void Draw(void* userdata);

  FML_DISALLOW_COPY_AND_ASSIGN(TextureTestEgl);

 private:
  [[maybe_unused]] bool m_initialized;

  WaylandEglBackend* m_egl_backend;

  static void Create(void* userdata);
  static void Dispose(void* userdata);
};
