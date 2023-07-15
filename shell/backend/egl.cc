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

#include "egl.h"

#include <cassert>
#include <cstring>

#include "logging.h"

Egl::Egl(void* native_display, EGLenum platform, int buffer_size, bool debug)
    : m_dpy(get_egl_display(platform, native_display, nullptr)),
      m_buffer_size(buffer_size) {
  assert(m_dpy);

  EGLBoolean ret = eglInitialize(m_dpy, &m_major, &m_minor);
  assert(ret == EGL_TRUE);
  SPDLOG_DEBUG("EGL {}.{}", m_major, m_minor);

  ret = eglBindAPI(EGL_OPENGL_ES_API);
  assert(ret == EGL_TRUE);

  EGL_KHR_debug_init();

  EGLint count;
  eglGetConfigs(m_dpy, nullptr, 0, &count);
  assert(count);
  SPDLOG_DEBUG("EGL has {} configs", count);

  auto* configs = reinterpret_cast<EGLConfig*>(
      calloc(static_cast<size_t>(count), sizeof(EGLConfig)));
  assert(configs);

  if (debug) {
    ReportGlesAttributes(configs, count);
  }

  EGLint n;
  ret = eglChooseConfig(m_dpy, kEglConfigAttribs.data(), configs, count, &n);
  assert(ret && n >= 1);

  EGLint size;
  for (EGLint i = 0; i < n; i++) {
    eglGetConfigAttrib(m_dpy, configs[i], EGL_BUFFER_SIZE, &size);
    SPDLOG_DEBUG("Buffer size for config {} is {}", i, size);
    if (m_buffer_size <= size) {
      m_config = configs[i];
      break;
    }
  }
  free(configs);
  if (m_config == nullptr) {
    spdlog::critical("did not find config with buffer size {}", m_buffer_size);
    assert(false);
  }

  m_context = eglCreateContext(m_dpy, m_config, EGL_NO_CONTEXT,
                               kEglContextAttribs.data());
  assert(m_context);

  m_resource_context =
      eglCreateContext(m_dpy, m_config, m_context, kEglContextAttribs.data());
  assert(m_resource_context);

  m_texture_context =
      eglCreateContext(m_dpy, m_config, m_context, kEglContextAttribs.data());

  ClearCurrent();

  (void)ret;
}

Egl::~Egl() {
  eglTerminate(m_dpy);
  eglReleaseThread();
}

bool Egl::MakeCurrent() {
  auto res = eglMakeCurrent(m_dpy, m_egl_surface, m_egl_surface, m_context);
  if (res != EGL_TRUE) {
    EGLint egl_error = eglGetError();
    if (egl_error != EGL_SUCCESS) {
      spdlog::critical("Make current failed: {}", egl_error);
      assert(false);
    }
  }
  return true;
}

bool Egl::ClearCurrent() {
  auto res =
      eglMakeCurrent(m_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (res != EGL_TRUE) {
    EGLint egl_error = eglGetError();
    if (egl_error != EGL_SUCCESS) {
      spdlog::critical("Clear current failed: {}", egl_error);
      assert(false);
    }
  }
  return true;
}

bool Egl::SwapBuffers() {
  auto res = eglSwapBuffers(m_dpy, m_egl_surface);
  if (res != EGL_TRUE) {
    EGLint egl_error = eglGetError();
    if (egl_error != EGL_SUCCESS) {
      spdlog::critical("SwapBuffers failed: {}", egl_error);
      assert(false);
    }
  }
  return true;
}

bool Egl::MakeResourceCurrent() {
  auto res =
      eglMakeCurrent(m_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, m_resource_context);
  if (res != EGL_TRUE) {
    EGLint egl_error = eglGetError();
    if (egl_error != EGL_SUCCESS) {
      spdlog::critical("MakeResourceCurrent failed: {}", egl_error);
      assert(false);
    }
  }
  return true;
}

bool Egl::MakeTextureCurrent() {
  auto res =
      eglMakeCurrent(m_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, m_texture_context);
  if (res != EGL_TRUE) {
    EGLint egl_error = eglGetError();
    if (egl_error != EGL_SUCCESS) {
      spdlog::critical("MakeTextureCurrent failed: {}", egl_error);
      assert(false);
    }
  }
  return false;
}

bool Egl::HasExtension(const char* name) {
  static const char* extensions = nullptr;
  if (extensions == nullptr) {
    extensions = eglQueryString(m_dpy, EGL_EXTENSIONS);
  }
  const char* r = strstr(extensions, name);
  auto len = strlen(name);
  // check that the extension name is terminated by space or null terminator
  return r != nullptr && (r[len] == ' ' || r[len] == 0);
}

std::array<EGLint, 4> Egl::RectToInts(const FlutterRect rect) const {
  EGLint height;
  eglQuerySurface(m_dpy, m_egl_surface, EGL_HEIGHT, &height);

  std::array<EGLint, 4> res{
      static_cast<int>(rect.left), height - static_cast<int>(rect.bottom),
      static_cast<int>(rect.right) - static_cast<int>(rect.left),
      static_cast<int>(rect.bottom) - static_cast<int>(rect.top)};
  return res;
}

// Print a list of extensions, with word-wrapping.
void Egl::print_extension_list(EGLDisplay dpy) {
  const char indentString[] = "\t    ";
  const int indent = 4;
  const int max = 79;
  int width, i, j;

  const char* ext = eglQueryString(dpy, EGL_EXTENSIONS);

  if (!ext || !ext[0])
    return;

  width = indent;
  std::stringstream ss;
  ss << indentString;
  i = j = 0;
  while (true) {
    if (ext[j] == ' ' || ext[j] == 0) {
      /* found end of an extension name */
      const int len = j - i;
      if (width + len > max) {
        /* start a new line */
        spdlog::info(ss.str().c_str());
        ss.str("");
        ss.clear();
        width = indent;
        ss << indentString;
      }
      /* print the extension name between ext[i] and ext[j] */
      while (i < j) {
        ss << ext[i];
        i++;
      }
      /* either we're all done, or we'll continue with next extension */
      width += len + 1;
      if (ext[j] == 0) {
        break;
      } else {
        i++;
        j++;
        if (ext[j] == 0)
          break;
        ss << ", ";
        width += 2;
      }
    }
    j++;
  }
  spdlog::info(ss.str().c_str());
  ss.str("");
  ss.clear();
}

#define COUNT_OF(x) (sizeof(x) / sizeof(0 [x]))

struct egl_enum_item {
  EGLint id;
  const char* name;
};

struct egl_enum_item egl_enum_boolean[] = {
    {
        .id = EGL_TRUE,
        .name = "EGL_TRUE",
    },
    {
        .id = EGL_FALSE,
        .name = "EGL_FALSE",
    },
};

struct egl_enum_item egl_enum_caveat[] = {
    {
        .id = EGL_NONE,
        .name = "EGL_NONE",
    },
    {
        .id = EGL_SLOW_CONFIG,
        .name = "EGL_SLOW_CONFIG",
    },
    {
        .id = EGL_NON_CONFORMANT_CONFIG,
        .name = "EGL_NON_CONFORMANT_CONFIG",
    },
};

struct egl_enum_item egl_enum_transparency[] = {
    {
        .id = EGL_NONE,
        .name = "EGL_NONE",
    },
    {
        .id = EGL_TRANSPARENT_RGB,
        .name = "EGL_TRANSPARENT_RGB",
    },
};

struct egl_enum_item egl_enum_color_buffer[] = {
    {
        .id = EGL_RGB_BUFFER,
        .name = "EGL_RGB_BUFFER",
    },
    {
        .id = EGL_LUMINANCE_BUFFER,
        .name = "EGL_LUMINANCE_BUFFER",
    },
};

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x40
#endif

struct egl_enum_item egl_enum_conformant[] = {
    {
        .id = EGL_OPENGL_BIT,
        .name = "EGL_OPENGL_BIT",
    },
    {
        .id = EGL_OPENGL_ES_BIT,
        .name = "EGL_OPENGL_ES_BIT",
    },
    {
        .id = EGL_OPENGL_ES2_BIT,
        .name = "EGL_OPENGL_ES2_BIT",
    },
    {
        .id = EGL_OPENGL_ES3_BIT,
        .name = "EGL_OPENGL_ES3_BIT",
    },
    {
        .id = EGL_OPENVG_BIT,
        .name = "EGL_OPENVG_BIT",
    },
};

struct egl_enum_item egl_enum_surface_type[] = {
    {
        .id = EGL_PBUFFER_BIT,
        .name = "EGL_PBUFFER_BIT",
    },
    {
        .id = EGL_PIXMAP_BIT,
        .name = "EGL_PIXMAP_BIT",
    },
    {
        .id = EGL_WINDOW_BIT,
        .name = "EGL_WINDOW_BIT",
    },
    {
        .id = EGL_VG_COLORSPACE_LINEAR_BIT,
        .name = "EGL_VG_COLORSPACE_LINEAR_BIT",
    },
    {
        .id = EGL_VG_ALPHA_FORMAT_PRE_BIT,
        .name = "EGL_VG_ALPHA_FORMAT_PRE_BIT",
    },
    {
        .id = EGL_MULTISAMPLE_RESOLVE_BOX_BIT,
        .name = "EGL_MULTISAMPLE_RESOLVE_BOX_BIT",
    },
    {
        .id = EGL_SWAP_BEHAVIOR_PRESERVED_BIT,
        .name = "EGL_SWAP_BEHAVIOR_PRESERVED_BIT",
    },
};

struct egl_enum_item egl_enum_renderable_type[] = {
    {
        .id = EGL_OPENGL_ES_BIT,
        .name = "EGL_OPENGL_ES_BIT",
    },
    {
        .id = EGL_OPENVG_BIT,
        .name = "EGL_OPENVG_BIT",
    },
    {
        .id = EGL_OPENGL_ES2_BIT,
        .name = "EGL_OPENGL_ES2_BIT",
    },
    {
        .id = EGL_OPENGL_BIT,
        .name = "EGL_OPENGL_BIT",
    },
    {
        .id = EGL_OPENGL_ES3_BIT,
        .name = "EGL_OPENGL_ES3_BIT",
    },
};

struct egl_config_attribute {
  EGLint id;
  const char* name;
  int32_t cardinality;
  const struct egl_enum_item* values;
};

static struct egl_config_attribute egl_config_attributes[] = {
    {
        .id = EGL_CONFIG_ID,
        .name = "EGL_CONFIG_ID",
    },
    {
        .id = EGL_CONFIG_CAVEAT,
        .name = "EGL_CONFIG_CAVEAT",
        .cardinality = COUNT_OF(egl_enum_caveat),
        .values = egl_enum_caveat,
    },
    {
        .id = EGL_LUMINANCE_SIZE,
        .name = "EGL_LUMINANCE_SIZE",
    },
    {
        .id = EGL_RED_SIZE,
        .name = "EGL_RED_SIZE",
    },
    {
        .id = EGL_GREEN_SIZE,
        .name = "EGL_GREEN_SIZE",
    },
    {
        .id = EGL_BLUE_SIZE,
        .name = "EGL_BLUE_SIZE",
    },
    {
        .id = EGL_ALPHA_SIZE,
        .name = "EGL_ALPHA_SIZE",
    },
    {
        .id = EGL_DEPTH_SIZE,
        .name = "EGL_DEPTH_SIZE",
    },
    {
        .id = EGL_STENCIL_SIZE,
        .name = "EGL_STENCIL_SIZE",
    },
    {
        .id = EGL_ALPHA_MASK_SIZE,
        .name = "EGL_ALPHA_MASK_SIZE",
    },
    {
        .id = EGL_BIND_TO_TEXTURE_RGB,
        .name = "EGL_BIND_TO_TEXTURE_RGB",
        .cardinality = COUNT_OF(egl_enum_boolean),
        .values = egl_enum_boolean,
    },
    {
        .id = EGL_BIND_TO_TEXTURE_RGBA,
        .name = "EGL_BIND_TO_TEXTURE_RGBA",
        .cardinality = COUNT_OF(egl_enum_boolean),
        .values = egl_enum_boolean,
    },
    {
        .id = EGL_MAX_PBUFFER_WIDTH,
        .name = "EGL_MAX_PBUFFER_WIDTH",
    },
    {
        .id = EGL_MAX_PBUFFER_HEIGHT,
        .name = "EGL_MAX_PBUFFER_HEIGHT",
    },
    {
        .id = EGL_MAX_PBUFFER_PIXELS,
        .name = "EGL_MAX_PBUFFER_PIXELS",
    },
    {
        .id = EGL_TRANSPARENT_RED_VALUE,
        .name = "EGL_TRANSPARENT_RED_VALUE",
    },
    {
        .id = EGL_TRANSPARENT_GREEN_VALUE,
        .name = "EGL_TRANSPARENT_GREEN_VALUE",
    },
    {
        .id = EGL_TRANSPARENT_BLUE_VALUE,
        .name = "EGL_TRANSPARENT_BLUE_VALUE",
    },
    {
        .id = EGL_SAMPLE_BUFFERS,
        .name = "EGL_SAMPLE_BUFFERS",
    },
    {
        .id = EGL_SAMPLES,
        .name = "EGL_SAMPLES",
    },
    {
        .id = EGL_LEVEL,
        .name = "EGL_LEVEL",
    },
    {
        .id = EGL_MAX_SWAP_INTERVAL,
        .name = "EGL_MAX_SWAP_INTERVAL",
    },
    {
        .id = EGL_MIN_SWAP_INTERVAL,
        .name = "EGL_MIN_SWAP_INTERVAL",
    },
    {
        .id = EGL_SURFACE_TYPE,
        .name = "EGL_SURFACE_TYPE",
        .cardinality = -(int32_t)COUNT_OF(egl_enum_surface_type),
        .values = egl_enum_surface_type,
    },
    {
        .id = EGL_RENDERABLE_TYPE,
        .name = "EGL_RENDERABLE_TYPE",
        .cardinality = -(int32_t)COUNT_OF(egl_enum_renderable_type),
        .values = egl_enum_renderable_type,
    },
    {
        .id = EGL_CONFORMANT,
        .name = "EGL_CONFORMANT",
        .cardinality = -(int32_t)COUNT_OF(egl_enum_conformant),
        .values = egl_enum_conformant,
    },
    {
        .id = EGL_TRANSPARENT_TYPE,
        .name = "EGL_TRANSPARENT_TYPE",
        .cardinality = COUNT_OF(egl_enum_transparency),
        .values = egl_enum_transparency,
    },
    {
        .id = EGL_COLOR_BUFFER_TYPE,
        .name = "EGL_COLOR_BUFFER_TYPE",
        .cardinality = COUNT_OF(egl_enum_color_buffer),
        .values = egl_enum_color_buffer,
    },
};

void Egl::ReportGlesAttributes(EGLConfig* configs, EGLint count) {
  std::stringstream ss;
  spdlog::info("OpenGL ES Attributes:");
  ss << "\tEGL_VENDOR: \"" << eglQueryString(m_dpy, EGL_VENDOR) << "\"";
  spdlog::info(ss.str().c_str());
  ss.str("");
  ss.clear();
  ss << "\tEGL_CLIENT_APIS: \"" << eglQueryString(m_dpy, EGL_CLIENT_APIS) << "\"";
  spdlog::info(ss.str().c_str());
  ss.str("");
  ss.clear();
  spdlog::info("\tEGL_EXTENSIONS:");

  print_extension_list(m_dpy);

  EGLint num_config;
  EGLBoolean status = eglGetConfigs(m_dpy, configs, count, &num_config);
  if (status != EGL_TRUE || num_config == 0) {
    spdlog::error("failed to get EGL frame buffer configurations");
    return;
  }

  spdlog::info("EGL framebuffer configurations:");
  for (EGLint i = 0; i < num_config; i++) {
    ss << "\tConfiguration #" << (int)i << ":";
    spdlog::info(ss.str().c_str());
    ss.str("");
    ss.clear();
    for (auto& egl_config_attribute : egl_config_attributes) {
      EGLint value = 0;
      eglGetConfigAttrib(m_dpy, configs[i], egl_config_attribute.id, &value);
      if (egl_config_attribute.cardinality == 0) {
        ss << "\t\t" << egl_config_attribute.name << ": " << (int)value;
        spdlog::info(ss.str().c_str());
        ss.str("");
        ss.clear();
      } else if (egl_config_attribute.cardinality > 0) {
        /* Enumeration */
        bool known_value = false;
        for (size_t k = 0; k < (size_t)egl_config_attribute.cardinality; k++) {
          if (egl_config_attribute.values[k].id == value) {
            ss << "\t\t" << egl_config_attribute.name << ": " << egl_config_attribute.values[k].name;
            spdlog::info(ss.str().c_str());
            ss.str("");
            ss.clear();
            known_value = true;
            break;
          }
        }
        if (!known_value) {
          ss << "\t\t" << egl_config_attribute.name << ": unknown (" << value << ")";
          spdlog::info(ss.str().c_str());
          ss.str("");
          ss.clear();
        }
      } else {
        /* Bitfield */
        ss << "\t\t" << egl_config_attribute.name << ": ";
        if (value == 0) {
          ss << "none";
          spdlog::info(ss.str().c_str());
          ss.str("");
          ss.clear();
        } else {
          for (size_t k = 0; k < (size_t)-egl_config_attribute.cardinality;
               k++) {
            if (egl_config_attribute.values[k].id & value) {
              value &= ~egl_config_attribute.values[k].id;
              if (value != 0) {
                ss << egl_config_attribute.values[k].name << " | ";
              } else {
                ss << egl_config_attribute.values[k].name;
                spdlog::info(ss.str().c_str());
                ss.str("");
                ss.clear();
              }
            }
          }
          if (value != 0) {
            ss << (int)value;
            spdlog::info(ss.str().c_str());
            ss.str("");
            ss.clear();
          }
        }
      }
    }
    spdlog::info(ss.str().c_str());
    ss.str("");
    ss.clear();
  }
}

void Egl::sDebugCallback(EGLenum error,
                         const char* command,
                         EGLint messageType,
                         EGLLabelKHR threadLabel,
                         EGLLabelKHR objectLabel,
                         const char* message) {
  spdlog::error("**** EGL Error");
  spdlog::error("\terror: {}", error);
  spdlog::error("\tcommand: {}", command);
  switch (error) {
    case EGL_BAD_ACCESS:
      spdlog::error("\terror: EGL_BAD_ACCESS");
      break;
    case EGL_BAD_ALLOC:
      spdlog::error("\terror: EGL_BAD_ALLOC");
      break;
    case EGL_BAD_ATTRIBUTE:
      spdlog::error("\terror: EGL_BAD_ATTRIBUTE");
      break;
    case EGL_BAD_CONFIG:
      spdlog::error("\terror: EGL_BAD_CONFIG");
      break;
    case EGL_BAD_CONTEXT:
      spdlog::error("\terror: EGL_BAD_CONTEXT");
      break;
    case EGL_BAD_CURRENT_SURFACE:
      spdlog::error("\terror: EGL_BAD_CURRENT_SURFACE");
      break;
    case EGL_BAD_DISPLAY:
      spdlog::error("\terror: EGL_BAD_DISPLAY");
      break;
    case EGL_BAD_MATCH:
      spdlog::error("\terror: EGL_BAD_MATCH");
      break;
    case EGL_BAD_NATIVE_PIXMAP:
      spdlog::error("\terror: EGL_BAD_NATIVE_PIXMAP");
      break;
    case EGL_BAD_NATIVE_WINDOW:
      spdlog::error("\terror: EGL_BAD_NATIVE_WINDOW");
      break;
    case EGL_BAD_PARAMETER:
      spdlog::error("\terror: EGL_BAD_PARAMETER");
      break;
    case EGL_BAD_SURFACE:
      spdlog::error("\terror: EGL_BAD_SURFACE");
      break;
    default:
      spdlog::error("\terror: {}", error);
      break;
  }
  spdlog::error("\tmessageType: {}", messageType);
  spdlog::error("\tthreadLabel: {}", threadLabel);
  spdlog::error("\tobjectLabel: {}", objectLabel);
  spdlog::error("\tmessage: {}", ((message == nullptr) ? "" : message));
}

void Egl::EGL_KHR_debug_init() {
  const char* extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

  if (extensions && (strstr(extensions, "EGL_KHR_debug"))) {
    SPDLOG_DEBUG("EGL_KHR_debug initialized");

    m_pfDebugMessageControl =
        (PFNEGLDEBUGMESSAGECONTROLKHRPROC)get_egl_proc_address(
            "eglDebugMessageControlKHR");
    assert(m_pfDebugMessageControl);

    const EGLAttrib sDebugAttribList[] = {EGL_DEBUG_MSG_CRITICAL_KHR,
                                          EGL_TRUE,
                                          EGL_DEBUG_MSG_ERROR_KHR,
                                          EGL_TRUE,
                                          EGL_DEBUG_MSG_WARN_KHR,
                                          EGL_TRUE,
                                          EGL_DEBUG_MSG_INFO_KHR,
                                          EGL_TRUE,
                                          EGL_NONE,
                                          0};

    m_pfDebugMessageControl(sDebugCallback, sDebugAttribList);

    m_pfQueryDebug =
        (PFNEGLQUERYDEBUGKHRPROC)get_egl_proc_address("eglQueryDebugKHR");
    assert(m_pfQueryDebug);

    m_pfLabelObject =
        (PFNEGLLABELOBJECTKHRPROC)get_egl_proc_address("eglLabelObjectKHR");
    assert(m_pfLabelObject);
  }
}

void* Egl::get_egl_proc_address(const char* address) {
  const char* extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

  if (extensions && (strstr(extensions, "EGL_EXT_platform_wayland") ||
                     strstr(extensions, "EGL_KHR_platform_wayland"))) {
    return (void*)eglGetProcAddress(address);
  }

  return nullptr;
}

EGLDisplay Egl::get_egl_display(EGLenum platform,
                                void* native_display,
                                const EGLint* attrib_list) {
  static PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = nullptr;
  static EGLDisplay dpy = nullptr;

  if (dpy != nullptr)
    return dpy;

  if (!get_platform_display) {
    get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)get_egl_proc_address(
            "eglGetPlatformDisplayEXT");
  }

  if (get_platform_display) {
    dpy = get_platform_display(platform, native_display, attrib_list);
    return dpy;
  }

  dpy = eglGetDisplay((EGLNativeDisplayType)native_display);
  return dpy;
}

EGLSurface Egl::create_egl_surface(void* native_window,
                                   const EGLint* attrib_list) {
  static PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_platform_window =
      nullptr;

  if (!create_platform_window) {
    create_platform_window =
        (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)get_egl_proc_address(
            "eglCreatePlatformWindowSurfaceEXT");
  }

  if (create_platform_window)
    return create_platform_window(m_dpy, m_config, native_window, attrib_list);

  return eglCreateWindowSurface(
      m_dpy, m_config, (EGLNativeWindowType)native_window, attrib_list);
}
