/*
 * Copyright © 2011 Benjamin Franzke
 * Copyright 2022 Toyota Connected North America
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "context.h"

#include <EGL/eglext.h>
#include <wayland-client.h>

#include <cassert>
#include <cstring>
#include <iostream>

uint32_t CompSurfContext::version() {
  return 0x00010000;
}

CompSurfContext::CompSurfContext(const char* accessToken,
                                 int width,
                                 int height,
                                 void* nativeWindow,
                                 const char* assetsPath,
                                 const char* cachePath,
                                 const char* miscPath)
    : mAccessToken(accessToken),
      mAssetsPath(assetsPath),
      mCachePath(cachePath),
      mMiscPath(miscPath),
      mWindow{nullptr},
      mDisplay{nullptr} {
  std::cout << "[comp_surf_egl]" << std::endl;
  std::cout << "assetsPath: " << mAssetsPath << std::endl;
  std::cout << "cachePath: " << mCachePath << std::endl;
  std::cout << "miscPath: " << mMiscPath << std::endl;

  mDisplay.window = &mWindow;
  mWindow.display = &mDisplay;
  mWindow.window_size.width = width;
  mWindow.window_size.height = height;
  mWindow.geometry.width = width;
  mWindow.geometry.height = height;

  typedef struct {
    struct wl_display* wl_display;
    struct wl_surface* wl_surface;
    EGLDisplay egl_display;
    struct wl_egl_window* egl_window;
  } wl;

  auto p = reinterpret_cast<wl*>(nativeWindow);
  assert(p);

  mWindow.surface = p->wl_surface;
  mDisplay.egl.dpy = p->egl_display;
  mWindow.native = p->egl_window;

  assert(mWindow.surface);
  assert(mDisplay.egl.dpy);
  assert(mWindow.native);

  init_egl(p->egl_window, p->egl_display, mWindow.egl_surface,
           mDisplay.egl.ctx);

  eglMakeCurrent(mDisplay.egl.dpy, mWindow.egl_surface, mWindow.egl_surface,
                 mDisplay.egl.ctx);

  init_gl(&mWindow);

  eglSwapInterval(mDisplay.egl.dpy, 0);

  eglMakeCurrent(mDisplay.egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                 EGL_NO_CONTEXT);
}

void CompSurfContext::de_initialize() const {
  eglMakeCurrent(mDisplay.egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                 EGL_NO_CONTEXT);
  eglReleaseThread();
}

void CompSurfContext::run_task() {}

void CompSurfContext::resize(int width, int height) {
  mWindow.geometry.width = width;
  mWindow.geometry.height = height;

  mWindow.window_size = mWindow.geometry;
}

void CompSurfContext::init_egl(void* nativeWindow,
                               EGLDisplay& eglDisplay,
                               EGLSurface& eglSurface,
                               EGLContext& eglContext) {
  EGLint config_attribs[] = {
      // clang-format off
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 1,
            EGL_GREEN_SIZE, 1,
            EGL_BLUE_SIZE, 1,
            EGL_ALPHA_SIZE, 1,
            EGL_NONE
      // clang-format on
  };

  static const EGLint context_attribs[] = {
      // clang-format off
            EGL_CONTEXT_MAJOR_VERSION, 2,
            EGL_NONE
      // clang-format on
  };

  EGLint major, minor;
  EGLBoolean ret = eglInitialize(eglDisplay, &major, &minor);
  assert(ret == EGL_TRUE);
  std::cout << "EGL major: " << major << ", minor: " << minor << std::endl;

  ret = eglBindAPI(EGL_OPENGL_ES_API);
  assert(ret == EGL_TRUE);

  EGLint count;
  eglGetConfigs(eglDisplay, nullptr, 0, &count);
  assert(count);

  auto* configs =
      reinterpret_cast<EGLConfig*>(calloc(count, sizeof(EGLConfig)));
  assert(configs);

  EGLint n;
  ret = eglChooseConfig(eglDisplay, config_attribs, configs, count, &n);
  assert(ret && n >= 1);

  EGLint size;
  EGLConfig egl_conf = nullptr;
  for (EGLint i = 0; i < n; i++) {
    eglGetConfigAttrib(eglDisplay, configs[i], EGL_BUFFER_SIZE, &size);
    if (kEglBufferSize <= size) {
      egl_conf = configs[i];
      break;
    }
  }
  free(configs);
  if (egl_conf == nullptr) {
    assert(false);
  }

  eglContext =
      eglCreateContext(eglDisplay, egl_conf, EGL_NO_CONTEXT, context_attribs);

  eglSurface = eglCreateWindowSurface(
      eglDisplay, egl_conf, (EGLNativeWindowType)nativeWindow, nullptr);
  assert(eglSurface != EGL_NO_SURFACE);
}

GLuint CompSurfContext::create_shader(const char* source, GLenum shader_type) {
  GLuint shader;
  GLint status;

  shader = glCreateShader(shader_type);
  assert(shader != 0);

  glShaderSource(shader, 1, (const char**)&source, nullptr);
  glCompileShader(shader);

  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (!status) {
    char log[1000];
    GLsizei len;
    glGetShaderInfoLog(shader, 1000, &len, log);
    fprintf(stderr, "Error: compiling %s: %*s\n",
            shader_type == GL_VERTEX_SHADER ? "vertex" : "fragment", len, log);
    exit(1);
  }

  return shader;
}

void CompSurfContext::init_gl(CompSurfContext::window* window) {
  GLuint frag, vert;
  GLuint program;
  GLint status;

  frag = create_shader(kFragShaderText, GL_FRAGMENT_SHADER);
  vert = create_shader(kVertShaderText, GL_VERTEX_SHADER);

  program = glCreateProgram();
  glAttachShader(program, frag);
  glAttachShader(program, vert);
  glLinkProgram(program);

  glGetProgramiv(program, GL_LINK_STATUS, &status);
  if (!status) {
    char log[1000];
    GLsizei len;
    glGetProgramInfoLog(program, 1000, &len, log);
    fprintf(stderr, "Error: linking:\n%*s\n", len, log);
    exit(1);
  }

  glUseProgram(program);

  window->gl.pos = 0;
  window->gl.col = 1;

  glBindAttribLocation(program, window->gl.pos, "pos");
  glBindAttribLocation(program, window->gl.col, "color");
  glLinkProgram(program);

  window->gl.rotation_uniform = glGetUniformLocation(program, "rotation");
}

void CompSurfContext::draw_frame(CompSurfContext* ctx, uint32_t time) const {
  static const GLfloat verts[3][2] = {{-0.5, -0.5}, {0.5, -0.5}, {0, 0.5}};
  static const GLfloat colors[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  GLfloat angle;
  GLfloat rotation[4][4] = {
      {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
  static const uint32_t speed_div = 5;
  static uint32_t start_time = 0;

  if (start_time == 0)
    start_time = time;

  angle = ((time - start_time) / speed_div) % 360 * M_PI / 180.0;
  rotation[0][0] = cos(angle);
  rotation[0][2] = sin(angle);
  rotation[2][0] = -sin(angle);
  rotation[2][2] = cos(angle);

  eglMakeCurrent(mDisplay.egl.dpy, mWindow.egl_surface, mWindow.egl_surface,
                 mDisplay.egl.ctx);

  glViewport(0, 0, mWindow.geometry.width, mWindow.geometry.height);

  glUniformMatrix4fv(mWindow.gl.rotation_uniform, 1, GL_FALSE,
                     (GLfloat*)rotation);

  glClearColor(0.0, 0.0, 0.0, 0.5);
  glClear(GL_COLOR_BUFFER_BIT);

  glVertexAttribPointer(mWindow.gl.pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
  glVertexAttribPointer(mWindow.gl.col, 3, GL_FLOAT, GL_FALSE, 0, colors);
  glEnableVertexAttribArray(mWindow.gl.pos);
  glEnableVertexAttribArray(mWindow.gl.col);

  glDrawArrays(GL_TRIANGLES, 0, 3);

  glDisableVertexAttribArray(mWindow.gl.pos);
  glDisableVertexAttribArray(mWindow.gl.col);

  eglSwapBuffers(mWindow.display->egl.dpy, mWindow.egl_surface);

  eglMakeCurrent(mDisplay.egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                 EGL_NO_CONTEXT);
}
