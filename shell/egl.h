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

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <wayland-egl.h>

#include "constants.h"

class Display;

class Egl {
 public:
  explicit Egl(wl_display* native_display,
               bool debug_egl,
               int buffer_size = kEglBufferSize);
  ~Egl();
  Egl(const Egl&) = delete;
  const Egl& operator=(const Egl&) = delete;

  [[maybe_unused]]
  EGLSurface GetSurface(size_t index) { return m_egl_surface[index]; }

  bool ClearCurrent();
  bool MakeCurrent(size_t index);
  bool SwapBuffers(size_t index);
  bool MakeResourceCurrent(size_t index);
  bool MakeTextureCurrent();

 protected:
  EGLSurface m_egl_surface[kEngineInstanceCount]{};
  EGLDisplay m_dpy;
  EGLConfig m_config{};
  EGLContext m_texture_context;

  static EGLSurface create_egl_surface(Egl* egl,
                                       void* native_window,
                                       const EGLint* attrib_list);

 private:
  wl_display* m_native_display;

  EGLContext m_context[kEngineInstanceCount]{};
  EGLContext m_resource_context[kEngineInstanceCount]{};

  EGLint m_major{};
  EGLint m_minor{};

  int m_buffer_size;

  PFNEGLDEBUGMESSAGECONTROLKHRPROC m_pfDebugMessageControl;
  PFNEGLQUERYDEBUGKHRPROC m_pfQueryDebug;
  PFNEGLLABELOBJECTKHRPROC m_pfLabelObject;

  [[maybe_unused]] void ReportGlesAttributes(EGLConfig* configs, EGLint count);

  static void* get_egl_proc_address(const char* address);

  static EGLDisplay get_egl_display(EGLenum platform,
                                    void* native_display,
                                    const EGLint* attrib_list);

  void EGL_KHR_debug_init();
};