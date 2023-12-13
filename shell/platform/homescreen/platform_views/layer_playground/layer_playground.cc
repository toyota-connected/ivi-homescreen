/*
* Copyright 2020-2023 Toyota Connected North America
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

#include "layer_playground.h"

#include <EGL/eglext.h>

#include <spdlog/spdlog.h>
#include <wayland-egl-core.h>
#include <wayland/display.h>

LayerPlayground::LayerPlayground(const int32_t id,
                                 std::string viewType,
                                 const int32_t direction,
                                 const double width,
                                 const double height,
                                 FlutterView* view)
    : PlatformView(id, std::move(viewType), direction, width, height),
      callback_(nullptr) {
  SPDLOG_TRACE("LayerPlayground: [{}] {}", GetViewType().c_str(), id);

  const auto display = view->GetDisplay();
  display_ = display->GetDisplay();
  surface_ = wl_compositor_create_surface(display->GetCompositor());
  parent_surface_ = view->GetWindow()->GetBaseSurface();
  subsurface_ = wl_subcompositor_get_subsurface(display->GetSubCompositor(),
                                                surface_, parent_surface_);

  egl_window_ = wl_egl_window_create(surface_, width_, height_);
  assert(egl_window_);

  egl_display_ = eglGetDisplay(display->GetDisplay());
  assert(egl_display_);

  InitializeEGL();
  egl_surface_ = eglCreateWindowSurface(
      egl_display_, egl_config_, reinterpret_cast<EGLNativeWindowType>(egl_window_), nullptr);

  // Sync
  //wl_subsurface_set_sync(subsurface_);
  wl_subsurface_set_desync(subsurface_);

  eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_);
  InitializeScene();
}

LayerPlayground::~LayerPlayground() {
  SPDLOG_TRACE("~LayerPlayground: [{}] {}", GetViewType().c_str(), GetId());
}

void LayerPlayground::Resize(const double width, const double height) {
  width_ = static_cast<int32_t>(width);
  height_ = static_cast<int32_t>(height);
  SPDLOG_TRACE("Resize: {} {}", width, height);
}

void LayerPlayground::SetDirection(const int32_t direction) {
  direction_ = direction;
  SPDLOG_TRACE("SetDirection: {}", direction_);
}

void LayerPlayground::SetOffset(const double left, const double top) {
  left_ = static_cast<int32_t>(left);
  top_ = static_cast<int32_t>(top);
  if (subsurface_) {
    SPDLOG_TRACE("SetOffset: left: {}, top: {}", left_, top_);
    wl_subsurface_set_position(subsurface_, left_, top_);
    if (!callback_) {
      on_frame(this, callback_, 0);
    }
  }
}

void LayerPlayground::Dispose(const bool /* hybrid */) {
  if (callback_) {
    wl_callback_destroy(callback_);
    callback_ = nullptr;
  }

  if (subsurface_) {
    wl_subsurface_destroy(subsurface_);
    subsurface_ = nullptr;
  }

  if (egl_window_) {
    wl_egl_window_destroy(egl_window_);
    egl_window_ = nullptr;
  }

  if (surface_) {
    wl_surface_destroy(surface_);
    surface_ = nullptr;
  }
}

void LayerPlayground::on_frame(void* data,
                               wl_callback* callback,
                               const uint32_t time) {
  const auto obj = static_cast<LayerPlayground*>(data);

  obj->callback_ = nullptr;

  if (callback) {
    wl_callback_destroy(callback);
  }

  obj->DrawFrame(time);

  // Z-Order
  //wl_subsurface_place_above(obj->subsurface_, obj->parent_surface_);
  wl_subsurface_place_below(obj->subsurface_, obj->parent_surface_);

  obj->callback_ = wl_surface_frame(obj->surface_);
  wl_callback_add_listener(obj->callback_, &LayerPlayground::frame_listener,
                           data);

  wl_subsurface_set_position(obj->subsurface_, obj->left_, obj->top_);

  wl_surface_commit(obj->surface_);
}

const wl_callback_listener LayerPlayground::frame_listener = {.done = on_frame};

GLuint LoadShader(const GLchar* shaderSrc, const GLenum type) {
  // Create the shader object
  const GLuint shader = glCreateShader(type);
  if (shader == 0)
    return 0;
  glShaderSource(shader, 1, &shaderSrc, nullptr);
  glCompileShader(shader);
  GLint compiled;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (!compiled) {
    GLint infoLen = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
    if (infoLen > 1) {
      auto* infoLog = static_cast<GLchar*>(malloc(sizeof(char) * infoLen));
      glGetShaderInfoLog(shader, infoLen, nullptr, infoLog);
      spdlog::error("Error compiling shader:\n{}\n", infoLog);
      free(infoLog);
    }
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

void LayerPlayground::InitializeEGL() {
  EGLint major, minor;
  EGLBoolean ret = eglInitialize(egl_display_, &major, &minor);
  assert(ret == EGL_TRUE);

  ret = eglBindAPI(EGL_OPENGL_ES_API);
  assert(ret == EGL_TRUE);

  EGLint count;
  eglGetConfigs(egl_display_, nullptr, 0, &count);
  assert(count);
  SPDLOG_TRACE("EGL has {} configs", count);

  auto* configs = static_cast<EGLConfig*>(
      calloc(static_cast<size_t>(count), sizeof(EGLConfig)));
  assert(configs);

  EGLint n;
  ret = eglChooseConfig(egl_display_, kEglConfigAttribs.data(), configs, count,
                        &n);
  assert(ret && n >= 1);

  EGLint size;
  for (EGLint i = 0; i < n; i++) {
    eglGetConfigAttrib(egl_display_, configs[i], EGL_BUFFER_SIZE, &size);
    SPDLOG_TRACE("Buffer size for config {} is {}", i, size);
    if (buffer_size_ <= size) {
      memcpy(&egl_config_, &configs[i], sizeof(EGLConfig));
      break;
    }
  }
  free(configs);
  if (egl_config_ == nullptr) {
    SPDLOG_CRITICAL("did not find config with buffer size {}", buffer_size_);
    assert(false);
  }

  egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT,
                                  kEglContextAttribs.data());
  assert(egl_context_);
  SPDLOG_TRACE("Context={}", egl_context_);
}

void LayerPlayground::InitializeScene() {
  constexpr GLchar vShaderStr[] =
      "attribute vec4 vPosition; \n"
      "void main() \n"
      "{ \n"
      " gl_Position = vPosition; \n"
      "} \n";
  constexpr GLchar fShaderStr[] =
      "precision mediump float; \n"
      "void main() \n"
      "{ \n"
      " gl_FragColor = vec4(1.5, 0.0, 0.0, 1.0); \n"
      "} \n";

  const GLuint vertexShader = LoadShader(vShaderStr, GL_VERTEX_SHADER);
  const GLuint fragmentShader = LoadShader(fShaderStr, GL_FRAGMENT_SHADER);

  const GLuint programObject = glCreateProgram();
  if (programObject == 0)
    return;

  glAttachShader(programObject, vertexShader);
  glAttachShader(programObject, fragmentShader);

  glBindAttribLocation(programObject, 0, "vPosition");

  glLinkProgram(programObject);

  GLint linked;
  glGetProgramiv(programObject, GL_LINK_STATUS, &linked);
  if (!linked) {
    GLint infoLen = 0;
    glGetProgramiv(programObject, GL_INFO_LOG_LENGTH, &infoLen);
    if (infoLen > 1) {
      auto* infoLog = static_cast<GLchar*>(malloc(sizeof(char) * infoLen));
      glGetProgramInfoLog(programObject, infoLen, nullptr, infoLog);
      spdlog::error("Error linking program:\n{}\n", infoLog);
      free(infoLog);
    }
    glDeleteProgram(programObject);
    return;
  }

  programObject_ = programObject;
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void LayerPlayground::DrawFrame(uint32_t /* time */) const {
  static constexpr GLfloat vVertices[] = {0.0f, 0.5f, 0.0f,  -0.5f, -0.5f,
                                          0.0f, 0.5f, -0.5f, 0.0f};

  if (eglGetCurrentContext() != egl_context_) {
    eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_);
  }

  glViewport(0, 0, width_, height_);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(programObject_);

  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, vVertices);
  glEnableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  eglSwapBuffers(egl_display_, egl_surface_);

  eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}
