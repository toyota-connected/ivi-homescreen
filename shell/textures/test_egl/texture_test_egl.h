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

#include <shell/platform/embedder/embedder.h>

#include "flutter/fml/macros.h"
#include "textures/texture.h"

class App;

class Backend;

class Engine;

class FlutterView;

class WaylandEglBackend;

class TextureTestEgl : public Texture {
 public:
  explicit TextureTestEgl(FlutterView* view);

  ~TextureTestEgl();

  /**
   * @brief Create test texture
   * @param[in,out] userdata Pointer to TextureTestEgl
   * @param[in] args from Dart
   * @return flutter::EncodableValue
   * @retval EncodableValue This contain result:OK, textureId, width, height,
   * GL_target, GL_format, GL_textureId
   * @retval Error
   * @relation
   * wayland, flutter
   */
  static flutter::EncodableValue Create(
      void* userdata,
      const std::map<flutter::EncodableValue, flutter::EncodableValue>* args);

  /**
   * @brief Dispose assigned EGL texture id
   * @param[in,out] userdata Pointer to TextureTestEgl
   * @param[in] name Texture2D id
   * @return void
   * @relation
   * wayland, flutter
   */
  static void Dispose(void* userdata, GLuint name);

  /**
   * @brief Draw a new texture
   * @param[in,out] userdata Pointer to TextureTestEgl
   * @return void
   * @relation
   * wayland, flutter
   */
  static void Draw(void* userdata);

  FML_DISALLOW_COPY_AND_ASSIGN(TextureTestEgl);

 private:
  bool m_initialized;

  WaylandEglBackend* m_egl_backend;
};
