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

#include <wayland-egl.h>

#include "constants.h"

#include "egl.h"
#include "engine.h"
#include "gl_process_resolver.h"

WaylandEglBackend::WaylandEglBackend(struct wl_display* display,
                                     uint32_t initial_width,
                                     uint32_t initial_height,
                                     bool debug_backend,
                                     int buffer_size)
    : Egl(display, EGL_PLATFORM_WAYLAND_KHR, buffer_size, debug_backend),
      Backend(this, Resize, CreateSurface),
      m_initial_width(initial_width),
      m_initial_height(initial_height) {}

FlutterRendererConfig WaylandEglBackend::GetRenderConfig() {
  return {
      .type = kOpenGL,
      .open_gl = {
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
          .fbo_callback = [](void*) -> uint32_t {
            return 0;  // FBO0
          },
          .make_resource_current = [](void* userdata) -> bool {
            auto e = reinterpret_cast<Engine*>(userdata);
            auto* b = (WaylandEglBackend*)(e->GetBackend());
            return b->MakeResourceCurrent();
          },
          .fbo_reset_after_present = true,
          .gl_proc_resolver = [](void* /* userdata */, const char* name) -> void* {
            return GlProcessResolver::GetInstance().process_resolver(name);
          },
          .gl_external_texture_frame_callback =
              [](void* userdata, int64_t texture_id, size_t width,
                 size_t height, FlutterOpenGLTexture* texture_out) -> bool {
            auto e = reinterpret_cast<Engine*>(userdata);
            auto texture = e->GetTextureObj(texture_id);
            if (texture) {
              texture_out->name = static_cast<uint32_t>(texture_id);
              texture_out->width = width;
              texture_out->height = height;
#if defined(ENABLE_PLUGIN_OPENGL_TEXTURE)
              texture->GetFlutterOpenGLTexture(texture_out);
#endif
              return true;
            }
            return false;
          },
          .present_with_info = [](void* userdata,
                                  const FlutterPresentInfo* info) -> bool {
            auto e = reinterpret_cast<Engine*>(userdata);
            auto* b = (WaylandEglBackend*)(e->GetBackend());

            // Free the existing damage that was allocated to this frame.
            if (b->m_existing_damage_map[info->fbo_id] != nullptr) {
              free(b->m_existing_damage_map[info->fbo_id]);
              b->m_existing_damage_map[info->fbo_id] = nullptr;
            }

            // Retrieve the set damage region function.
            PFNEGLSETDAMAGEREGIONKHRPROC set_damage_region_ = nullptr;
            if (b->HasExtension("EGL_KHR_partial_update")) {
              set_damage_region_ =
                  reinterpret_cast<PFNEGLSETDAMAGEREGIONKHRPROC>(
                      GlProcessResolver::GetInstance().process_resolver(
                          "eglSetDamageRegionKHR"));
            }

            // Retrieve the swap buffers with damage function.
            PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC swap_buffers_with_damage_ =
                nullptr;
            if (b->HasExtension("EGL_EXT_swap_buffers_with_damage")) {
              swap_buffers_with_damage_ =
                  reinterpret_cast<PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC>(
                      GlProcessResolver::GetInstance().process_resolver(
                          "eglSwapBuffersWithDamageEXT"));
            } else if (b->HasExtension("EGL_KHR_swap_buffers_with_damage")) {
              swap_buffers_with_damage_ =
                  reinterpret_cast<PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC>(
                      GlProcessResolver::GetInstance().process_resolver(
                          "eglSwapBuffersWithDamageKHR"));
            }

            if (set_damage_region_) {
              // Set the buffer damage as the damage region.
              auto buffer_rects = b->RectToInts(info->buffer_damage.damage[0]);
              set_damage_region_(b->GetDisplay(), b->m_egl_surface,
                                 buffer_rects.data(), 1);
            }

            // Add frame damage to damage history
            b->m_damage_history.push_back(info->frame_damage.damage[0]);
            if (b->m_damage_history.size() > kMaxHistorySize) {
              b->m_damage_history.pop_front();
            }

            if (swap_buffers_with_damage_) {
              // Swap buffers with frame damage.
              auto frame_rects = b->RectToInts(info->frame_damage.damage[0]);
              return swap_buffers_with_damage_(
                  b->GetDisplay(), b->m_egl_surface, frame_rects.data(), 1);
            } else {
              // If the required extensions for partial repaint were not
              // provided, do full repaint.
              return b->SwapBuffers();
            }
          },
          .populate_existing_damage =
              [](void* userdata, intptr_t fbo_id,
                 FlutterDamage* existing_damage) -> void {
            auto e = reinterpret_cast<Engine*>(userdata);
            auto* b = (WaylandEglBackend*)(e->GetBackend());
            // Given the FBO age, create existing damage region by joining all
            // frame damages since FBO was last used
            EGLint age;
            if (b->HasExtension("GL_EXT_buffer_age")) {
              eglQuerySurface(b->GetDisplay(), b->m_egl_surface,
                              EGL_BUFFER_AGE_EXT, &age);
            } else {
              age = 4;  // Virtually no driver should have a swap chain length
                        // > 4.
            }

            existing_damage->num_rects = 1;

            // Allocate the array of rectangles for the existing damage.
            b->m_existing_damage_map[fbo_id] = static_cast<FlutterRect*>(
                malloc(sizeof(FlutterRect) * existing_damage->num_rects));
            b->m_existing_damage_map[fbo_id][0] =
                FlutterRect{0, 0, static_cast<double>(b->m_initial_width),
                            static_cast<double>(b->m_initial_height)};
            existing_damage->damage = b->m_existing_damage_map[fbo_id];

            if (age > 1) {
              --age;
              // join up to (age - 1) last rects from damage history
              for (auto i = b->m_damage_history.rbegin();
                   i != b->m_damage_history.rend() && age > 0; ++i, --age) {
                if (i == b->m_damage_history.rbegin()) {
                  if (i != b->m_damage_history.rend()) {
                    existing_damage->damage[0] = {i->left, i->top, i->right,
                                                  i->bottom};
                  }
                } else {
                  JoinFlutterRect(&(existing_damage->damage[0]), *i);
                }
              }
            }
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
                               size_t /* index */,
                               Engine* engine,
                               int32_t width,
                               int32_t height) {
  auto b = reinterpret_cast<WaylandEglBackend*>(user_data);
  if (b->m_egl_window) {
    if (engine) {
      auto result = engine->SetWindowSize(static_cast<size_t>(height),
                                          static_cast<size_t>(width));
      if (result != kSuccess) {
        spdlog::error("Failed to set Flutter Engine Window Size");
      }
    }
    wl_egl_window_resize(b->m_egl_window, width, height, 0, 0);
  }
}

void WaylandEglBackend::CreateSurface(void* user_data,
                                      size_t /* index */,
                                      wl_surface* surface,
                                      int32_t width,
                                      int32_t height) {
  auto b = reinterpret_cast<WaylandEglBackend*>(user_data);
  b->m_egl_window = wl_egl_window_create(surface, width, height);
  b->m_egl_surface = b->create_egl_surface(b->m_egl_window, nullptr);
}

void WaylandEglBackend::JoinFlutterRect(FlutterRect* rect,
                                        FlutterRect additional_rect) {
  rect->left = std::min(rect->left, additional_rect.left);
  rect->top = std::min(rect->top, additional_rect.top);
  rect->right = std::max(rect->right, additional_rect.right);
  rect->bottom = std::max(rect->bottom, additional_rect.bottom);
}
