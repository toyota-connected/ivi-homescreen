/*
 * Copyright 2021-2024 Toyota Connected North America
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

#include "osmesa.h"

#include <cassert>

#include <GLES2/gl2.h>

#include "constants.h"
#include "logging.h"

OSMesaHeadless::OSMesaHeadless(int32_t initial_width, int32_t initial_height)
    : m_width(initial_width), m_height(initial_height) {
  if (initial_width == 0) {
    m_width = kDefaultViewWidth;
  }
  if (initial_height == 0) {
    m_height = kDefaultViewHeight;
  }

  m_context = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0, nullptr);
  assert(m_context);
  spdlog::trace("Context Created");

  m_resource_context = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0, m_context);
  assert(m_resource_context);
  spdlog::trace("Resource Context Created");

  m_texture_context = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0, m_context);
  assert(m_texture_context);
  spdlog::trace("Texture Context Created");

  m_buf = create_osmesa_buffer(m_width, m_height);
  MakeCurrent();
}

OSMesaHeadless::~OSMesaHeadless() {
  OSMesaDestroyContext(m_context);
}

bool OSMesaHeadless::MakeCurrent() {
  spdlog::trace("+MakeCurrent(), thread_id=0x{:x}", pthread_self());
  bool ret =
      OSMesaMakeCurrent(m_context, m_buf, GL_UNSIGNED_BYTE, m_width, m_height);
  assert(ret);
  spdlog::trace("-MakeCurrent()");
  return true;
}

bool OSMesaHeadless::ClearCurrent() const {
  spdlog::trace("+ClearCurrent(), thread_id=0x{:x}", pthread_self());
  bool ret =
      OSMesaMakeCurrent(nullptr, nullptr, GL_UNSIGNED_BYTE, m_width, m_height);
  assert(ret);
  spdlog::trace("-ClearCurrent()");
  return true;
}

bool OSMesaHeadless::MakeResourceCurrent() {
  spdlog::trace("+MakeResourceCurrent(), thread_id=0x{:x}", pthread_self());
  bool ret = OSMesaMakeCurrent(m_resource_context, m_buf, GL_UNSIGNED_BYTE,
                               m_width, m_height);
  assert(ret);
  spdlog::trace("-MakeResourceCurrent()");
  return true;
}

bool OSMesaHeadless::MakeTextureCurrent() {
  spdlog::trace("+MakeTextureCurrent(), thread_id=0x{:x}", pthread_self());
  bool ret = OSMesaMakeCurrent(m_texture_context, m_buf, GL_UNSIGNED_BYTE,
                               m_width, m_height);
  assert(ret);
  spdlog::trace("-MakeTextureCurrent()");
  return true;
}

void OSMesaHeadless::Finish() {
  glFinish();
}

GLubyte* OSMesaHeadless::create_osmesa_buffer(int32_t width, int32_t height) {
  return (GLubyte*)malloc(static_cast<unsigned long>(height * width * 4) *
                          sizeof(GLubyte));
}

void OSMesaHeadless::free_buffer() {
  free(m_buf);
}
