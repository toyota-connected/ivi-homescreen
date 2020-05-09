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

#include <GLES3/gl3.h>

#include <flutter_embedder.h>

#include "flutter/fml/macros.h"
#include "textures/texture.h"

class App;
class EglWindow;
class Engine;

class TextureTest : public Texture {
 public:
  explicit TextureTest(App* app);
  ~TextureTest();

  void Draw(void* userdata);

  FML_DISALLOW_COPY_AND_ASSIGN(TextureTest);

 private:
  [[maybe_unused]] bool m_initialized;

  std::shared_ptr<EglWindow> m_egl_window;

  static void Create(void* userdata);
  static void Dispose(void* userdata);
};
