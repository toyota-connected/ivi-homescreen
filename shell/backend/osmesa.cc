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

#include "osmesa.h"

#include <cassert>
#include <cstring>
#include <sstream>

#include <GLES2/gl2.h>

#include "osmesa_process_resolver.h"
#include "logging.h"

OSMesaHeadless::OSMesaHeadless() {
  m_context = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0, NULL);
}

OSMesaHeadless::~OSMesaHeadless() {
  OSMesaDestroyContext(m_context);
}

bool OSMesaHeadless::MakeCurrent() {
  SPDLOG_TRACE("+MakeCurrent(), thread_id=0x{:x}", pthread_self());
  OSMesaMakeCurrent( m_context, m_buf, GL_UNSIGNED_BYTE, m_width, m_height );
  SPDLOG_TRACE("-MakeCurrent()");
  return true;
}

bool OSMesaHeadless::ClearCurrent() {
  SPDLOG_TRACE("+ClearCurrent(), thread_id=0x{:x}", pthread_self());
  OSMesaMakeCurrent( nullptr, nullptr, GL_UNSIGNED_BYTE, m_width, m_height );
  SPDLOG_TRACE("-ClearCurrent()");
  return true;
}

void OSMesaHeadless::Finish() {
    glFinish();
}

GLubyte* OSMesaHeadless::create_osmesa_buffer(int32_t width, int32_t height) {
  return (GLubyte*)malloc( height * width * 4 * sizeof(GLubyte) );
}

void OSMesaHeadless::free_buffer() {
  free(m_buf);
}

