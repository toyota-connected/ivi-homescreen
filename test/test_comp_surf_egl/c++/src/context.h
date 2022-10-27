/*
 * Copyright 2022 Toyota Connected North America
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

#include "../include/comp_surf_egl/comp_surf_egl.h"

#include <memory>
#include <string>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

struct CompSurfContext {
 public:
  struct window;

  struct display {
    struct {
      EGLDisplay dpy;
      EGLContext ctx;
      EGLConfig conf;
    } egl;
    struct window* window;
  };

  struct geometry {
    int width, height;
  };

  struct window {
    struct display* display;
    struct geometry geometry, window_size;
    struct {
      GLuint rotation_uniform;
      GLuint pos;
      GLuint col;
    } gl;

    struct wl_egl_window* native;
    struct wl_surface* surface;
    EGLSurface egl_surface;
    struct wl_callback* callback;
  };

  static uint32_t version();

  CompSurfContext(const char* accessToken,
                  int width,
                  int height,
                  void* nativeWindow,
                  const char* assetsPath,
                  comp_surf_CommitFrameFunction commit_frame,
                  void* commit_frame_user_data);

  ~CompSurfContext();

  CompSurfContext(const CompSurfContext&) = delete;

  CompSurfContext(CompSurfContext&&) = delete;

  CompSurfContext& operator=(const CompSurfContext&) = delete;

  void run_task();

  void resize(int width, int height);

  window* GetWindow() { return &window_; }

 private:
  static constexpr int kEglBufferSize = 16;
  std::string accessToken_;
  int width_;
  int height_;
  std::string assetsPath_;
  comp_surf_CommitFrameFunction commit_frame_;
  void* commit_frame_user_data_;

  struct {
    EGLDisplay dpy;
    EGLContext ctx;
    EGLConfig conf;
  } egl;

  static constexpr char kVertShaderText[] =
      "uniform mat4 rotation;\n"
      "attribute vec4 pos;\n"
      "attribute vec4 color;\n"
      "varying vec4 v_color;\n"
      "void main() {\n"
      "  gl_Position = rotation * pos;\n"
      "  v_color = color;\n"
      "}\n";

  static constexpr char kFragShaderText[] =
      "precision mediump float;\n"
      "varying vec4 v_color;\n"
      "void main() {\n"
      "  gl_FragColor = v_color;\n"
      "}\n";

  struct display display_;
  struct window window_;

  static void* get_egl_proc_address(const char* address);

  static EGLSurface create_egl_surface(EGLDisplay& eglDisplay,
                                       EGLConfig& eglConfig,
                                       void* native_window,
                                       const EGLint* attrib_list);

  static void init_egl(void* nativeWindow,
                       EGLDisplay& eglDisplay,
                       EGLSurface& eglSurface,
                       EGLContext& eglContext);

  static void fini_egl(struct display* display);

  static GLuint create_shader(const char* source, GLenum shader_type);

  static void init_gl(struct window* window);

  static void destroy_surface(struct window* window);

  static const struct wl_callback_listener frame_listener;

  static void redraw(void* data, struct wl_callback* callback, uint32_t time);
};
