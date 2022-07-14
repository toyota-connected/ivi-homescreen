/*
 * Copyright 2020-2022 Toyota Connected North America
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

#include "wayland_egl.h"

#include <cstring>

#include <wayland-egl.h>

#include "constants.h"
#include "third_party/flutter/fml/logging.h"

#include "egl.h"
#include "engine.h"
#include "gl_process_resolver.h"
#include "textures/texture.h"

WaylandEglBackend::WaylandEglBackend(struct wl_display* display,
                                     bool debug_backend,
                                     int buffer_size)
    : Egl(display, EGL_PLATFORM_WAYLAND_KHR, buffer_size, debug_backend),
      Backend(this, Resize, CreateSurface) {}

FlutterRendererConfig WaylandEglBackend::GetRenderConfig() {
  return {
      .type = kOpenGL,
      .open_gl{
          .struct_size = sizeof(FlutterOpenGLRendererConfig),
          .make_current = [](void* userdata) -> bool {
            auto e = reinterpret_cast<Engine*>(userdata);
            auto* b = (WaylandEglBackend*)(e->GetBackend());
            return b->MakeCurrent();
          },
          .clear_current = [](void* userdata) -> bool {
            auto e = reinterpret_cast<Engine*>(userdata);
            auto* b = (WaylandEglBackend*)(e->GetBackend());
            return b->ClearCurrent();
          },
          .present = [](void* userdata) -> bool {
            auto e = reinterpret_cast<Engine*>(userdata);
            auto* b = (WaylandEglBackend*)(e->GetBackend());
            return b->SwapBuffers();
          },
          .fbo_callback = [](void* userdata) -> uint32_t { return 0; },
          .make_resource_current = [](void* userdata) -> bool {
            auto e = reinterpret_cast<Engine*>(userdata);
            auto* b = (WaylandEglBackend*)(e->GetBackend());
            return b->MakeResourceCurrent();
          },
          .fbo_reset_after_present = false,
          .surface_transformation = nullptr,
          .gl_proc_resolver = [](void* userdata, const char* name) -> void* {
            return GlProcessResolver::GetInstance().process_resolver(name);
          },
          .gl_external_texture_frame_callback =
              [](void* userdata, int64_t texture_id, size_t width,
                 size_t height, FlutterOpenGLTexture* texture_out) -> bool {
            auto e = reinterpret_cast<Engine*>(userdata);
            auto texture = e->GetTextureObj(texture_id);
            if (texture) {
              texture->GetFlutterOpenGLTexture(texture_out,
                                               static_cast<int>(width),
                                               static_cast<int>(height));
              return true;
            }
            return false;
          },
      }};
}

FlutterCompositor WaylandEglBackend::GetCompositorConfig() {
  return {.struct_size = sizeof(FlutterCompositor),
          .user_data = this,
          .create_backing_store_callback = nullptr,
          .collect_backing_store_callback = nullptr,
          .present_layers_callback = nullptr,
          .avoid_backing_store_cache = true};
}

void WaylandEglBackend::Resize(void* user_data,
                               size_t index,
                               Engine* engine,
                               int32_t width,
                               int32_t height) {
  (void)index;
  auto b = reinterpret_cast<WaylandEglBackend*>(user_data);
  if (b->m_egl_window) {
    if (engine) {
      auto result = engine->SetWindowSize(height, width);
      if (result != kSuccess) {
        FML_LOG(ERROR) << "Failed to set Flutter Engine Window Size";
      }
    }
    wl_egl_window_resize(b->m_egl_window, width, height, 0, 0);
  }
}

void WaylandEglBackend::CreateSurface(void* user_data,
                                      size_t index,
                                      wl_surface* surface,
                                      int32_t width,
                                      int32_t height) {
  (void)index;
  auto b = reinterpret_cast<WaylandEglBackend*>(user_data);
  b->m_egl_window = wl_egl_window_create(surface, width, height);
  b->m_egl_surface = b->create_egl_surface(b->m_egl_window, nullptr);
}
