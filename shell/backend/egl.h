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

#include <cstdint>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "constants.h"

class Egl {
 public:
  Egl(void* native_display, EGLenum platform, int buffer_size, bool debug);

  ~Egl();

  bool ClearCurrent();

  bool MakeCurrent();

  bool SwapBuffers();

  bool MakeResourceCurrent();

  bool MakeTextureCurrent();

  static EGLDisplay get_egl_display(EGLenum platform,
                                    void* native_display,
                                    const EGLint* attrib_list);

  EGLSurface create_egl_surface(void* native_window, const EGLint* attrib_list);

  EGLSurface m_egl_surface{};

 private:
  EGLConfig m_config{};
  EGLContext m_texture_context;

  int m_buffer_size;

  EGLDisplay m_dpy;
  EGLContext m_context{};
  EGLContext m_resource_context{};

  EGLint m_major{};
  EGLint m_minor{};

  PFNEGLDEBUGMESSAGECONTROLKHRPROC m_pfDebugMessageControl{};
  PFNEGLQUERYDEBUGKHRPROC m_pfQueryDebug{};
  PFNEGLLABELOBJECTKHRPROC m_pfLabelObject{};

  void ReportGlesAttributes(EGLConfig* configs, EGLint count);

  static void sDebugCallback(EGLenum error,
                             const char* command,
                             EGLint messageType,
                             EGLLabelKHR threadLabel,
                             EGLLabelKHR objectLabel,
                             const char* message);

  static void* get_egl_proc_address(const char* address);

  void EGL_KHR_debug_init();

  static void print_extension_list(const char* ext);
};