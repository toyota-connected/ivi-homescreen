/*
 * Copyright 2021-2022 Toyota Connected North America
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

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <shell/platform/embedder/embedder.h>
#include <GL/osmesa.h>

#include "constants.h"

class OSMesaHeadless {
 public:
  OSMesaHeadless();

  ~OSMesaHeadless();

  /**
   * @brief Clear an EGL rendering context
   * @return bool
   * @retval true Normal end
   * @retval false Abnormal end
   * @relation
   * wayland
   */
  bool ClearCurrent();

  /**
   * @brief Attach an EGL rendering context to EGL surface
   * @return bool
   * @retval true Normal end
   * @retval false Abnormal end
   * @relation
   * wayland
   */
  bool MakeCurrent();

  void Finish();

  

  /**
   * @brief Create a GLUbyte buffer to bind to an OSMesa Context
   * @param[in] width
   * @param[in] height
   * @return GLubyte*
   * @retval GLUbyte buffer
   */
  GLubyte* create_osmesa_buffer(int32_t width, int32_t height);
  void free_buffer();

 protected:
  GLubyte* m_buf{};

 private:
  OSMesaContext m_context{};
  int32_t m_height{};
  int32_t m_width{};
};
