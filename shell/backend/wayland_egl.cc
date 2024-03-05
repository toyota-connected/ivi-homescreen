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

#include "egl.h"
#include "engine.h"
#include "gl_process_resolver.h"
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"

struct FlutterDesktopEngineState;

WaylandEglBackend::WaylandEglBackend(struct wl_display* display,
                                     const uint32_t initial_width,
                                     const uint32_t initial_height,
                                     const bool debug_backend,
                                     const int buffer_size)
    : Egl(display, buffer_size, debug_backend),
      Backend(),
      m_initial_width(initial_width),
      m_initial_height(initial_height) {}

FlutterRendererConfig WaylandEglBackend::GetRenderConfig() {
  return {
      .type = kOpenGL,
      .open_gl = {
          .struct_size = sizeof(FlutterOpenGLRendererConfig),
          .make_current = [](void* user_data) -> bool {
            const auto state =
                static_cast<FlutterDesktopEngineState*>(user_data);
            return reinterpret_cast<WaylandEglBackend*>(
                       state->view_controller->engine->GetBackend())
                ->MakeCurrent();
          },
          .clear_current = [](void* userdata) -> bool {
            const auto state =
                static_cast<FlutterDesktopEngineState*>(userdata);
            return reinterpret_cast<WaylandEglBackend*>(
                       state->view_controller->engine->GetBackend())
                ->ClearCurrent();
          },
          .fbo_callback = [](void*) -> uint32_t {
            return 0;  // FBO0
          },
          .make_resource_current = [](void* userdata) -> bool {
            const auto state =
                static_cast<FlutterDesktopEngineState*>(userdata);
            return reinterpret_cast<WaylandEglBackend*>(
                       state->view_controller->engine->GetBackend())
                ->MakeResourceCurrent();
          },
          .fbo_reset_after_present = false,
          .gl_proc_resolver = [](void* /* userdata */,
                                 const char* name) -> void* {
            return GlProcessResolver::GetInstance().process_resolver(name);
          },
          .gl_external_texture_frame_callback =
              [](void* userdata, const int64_t texture_id, const size_t width,
                 const size_t height,
                 FlutterOpenGLTexture* texture_out) -> bool {
            const auto state =
                static_cast<FlutterDesktopEngineState*>(userdata);
            auto& texture_registry = state->texture_registrar->texture_registry;
            auto it = std::find_if(
                std::begin(texture_registry), std::end(texture_registry),
                [&texture_id](auto&& p) { return p.first == texture_id; });
            // texture not found in registry
            if (it == std::end(texture_registry))
              return false;
            auto& target = texture_registry[texture_id];
            *texture_out = {.target = target->target,
                            .name = target->name,
                            .format = target->format,
                            .user_data = target->release_context,
                            .destruction_callback = target->release_callback,
                            .width = target->width,
                            .height = target->height};
            target->visible_width = width;
            target->visible_width = height;
            return true;
          },
          .present_with_info = [](void* userdata,
                                  const FlutterPresentInfo* info) -> bool {
            const auto state =
                static_cast<FlutterDesktopEngineState*>(userdata);
            auto* b = reinterpret_cast<WaylandEglBackend*>(
                state->view_controller->engine->GetBackend());

            // Full swap if FlutterPresentInfo is invalid
            if (info->struct_size != sizeof(FlutterPresentInfo)) {
              return b->SwapBuffers();
            }

            // Free the existing damage that was allocated to this frame.
            if (b->m_existing_damage_map[info->fbo_id] != nullptr) {
              free(b->m_existing_damage_map[info->fbo_id]);
              b->m_existing_damage_map[info->fbo_id] = nullptr;
            }

            if (b->GetSetDamageRegion()) {
              // Set the buffer damage as the damage region.
              auto buffer_rects = b->RectToInts(info->buffer_damage.damage[0]);
              b->GetSetDamageRegion()(b->GetDisplay(), b->m_egl_surface,
                                      buffer_rects.data(), 1);
            }

            // Add frame damage to damage history
            b->m_damage_history.push_back(info->frame_damage.damage[0]);
            if (b->m_damage_history.size() > kMaxHistorySize) {
              b->m_damage_history.pop_front();
            }

            if (b->GetSwapBuffersWithDamage()) {
              // Swap buffers with frame damage.
              const auto frame_rects =
                  b->RectToInts(info->frame_damage.damage[0]);
              return b->GetSwapBuffersWithDamage()(
                  b->GetDisplay(), b->m_egl_surface,
                  const_cast<int*>(frame_rects.data()), 1);
            } else {
              // If the required extensions for partial repaint were not
              // provided, do full repaint.
              return b->SwapBuffers();
            }
          },
          .populate_existing_damage =
              [](void* userdata, const intptr_t fbo_id,
                 FlutterDamage* existing_damage) -> void {
            const auto state =
                static_cast<FlutterDesktopEngineState*>(userdata);
            auto* b = reinterpret_cast<WaylandEglBackend*>(
                state->view_controller->engine->GetBackend());
            // Given the FBO age, create existing damage region by joining
            // all frame damages since FBO was last used
            EGLint age;
            if (b->HasExtBufferAge()) {
              eglQuerySurface(b->GetDisplay(), b->m_egl_surface,
                              EGL_BUFFER_AGE_EXT, &age);
            } else {
              age = 4;  // Virtually no driver should have a swap chain
                        // length > 4.
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

void WaylandEglBackend::Resize(size_t /* index */,
                               Engine* flutter_engine,
                               int32_t width,
                               int32_t height) {
  if (m_egl_window) {
    if (flutter_engine) {
      const auto result = flutter_engine->SetWindowSize(
          static_cast<size_t>(height), static_cast<size_t>(width));
      if (result != kSuccess) {
        spdlog::error("Failed to set Flutter Engine Window Size");
      }
    }
    UpdateSize(width, height);
    wl_egl_window_resize(m_egl_window, width, height, 0, 0);
  }
}

void WaylandEglBackend::CreateSurface(size_t /* index */,
                                      wl_surface* surface,
                                      int32_t width,
                                      int32_t height) {
  UpdateSize(width, height);
  m_egl_window = wl_egl_window_create(surface, width, height);
  m_egl_surface = create_egl_surface(m_egl_window, nullptr);
}

#if 0  // TODO
int64_t WaylandEglBackend::AddTexture() {
  MakeTextureCurrent();

  /// Create Texture
  GLuint texture_id;
  glGenTextures(1, &texture_id);
  SPDLOG_DEBUG("RegisterExternalTexture: {}", texture_id);

  /// check for danglers
  for (const auto& it : m_texture_registry) {
    if (it.first == texture_id) {
      DestroyTexture(texture_id);
      break;
    }
  }

  std::scoped_lock<std::mutex> lock(m_texture_mutex);
  m_texture_registry[texture_id] = std::make_unique<GL_TEXTURE_2D_DESC>();

  return texture_id;
}

void WaylandEglBackend::DestroyTexture(int64_t texture_id) {
  std::scoped_lock<std::mutex> lock(m_texture_mutex);
  m_texture_registry[texture_id].reset();
  m_texture_registry.erase(texture_id);
}

int64_t WaylandEglBackend::RegisterExternalTexture(
    FlutterDesktopTextureRegistrarRef /* texture_registrar */,
    const FlutterDesktopTextureInfo* texture_info) {
  int64_t result = -1;

  if (texture_info->type == kFlutterDesktopPixelBufferTexture) {
    spdlog::error("RegisterExternalTexture: Pixel Buffer not implemented yet");
  } else if (texture_info->type == kFlutterDesktopGpuSurfaceTexture) {
    auto surface_texture = texture_info->gpu_surface_config;
    if (surface_texture.type != kFlutterDesktopGpuSurfaceTypeGlTexture2D) {
      spdlog::error(
          "RegisterExternalTexture: kFlutterDesktopGpuSurfaceTypeGlTexture2D "
          "is only supported at this time");
      return result;
    }

    return AddTexture();
  }
  return result;
}

void WaylandEglBackend::UnregisterExternalTexture(
    FlutterDesktopTextureRegistrarRef texture_registrar,
    int64_t texture_id,
    void (*callback)(void* user_data),
    void* callback_context) {
  MakeTextureCurrent();
  // tear down texture_id here
  std::scoped_lock<std::mutex> lock(m_texture_mutex);
  m_texture_registry[texture_id] = std::make_unique<GL_TEXTURE_2D_DESC>();
  callback(callback_context);
  DestroyTexture(texture_id);
}

#endif  // TODO

bool WaylandEglBackend::TextureMakeCurrent() {
  return MakeTextureCurrent();
}

bool WaylandEglBackend::TextureClearCurrent() {
  return ClearCurrent();
}

void WaylandEglBackend::JoinFlutterRect(FlutterRect* rect,
                                        const FlutterRect& additional_rect) {
  rect->left = std::min(rect->left, additional_rect.left);
  rect->top = std::min(rect->top, additional_rect.top);
  rect->right = std::max(rect->right, additional_rect.right);
  rect->bottom = std::max(rect->bottom, additional_rect.bottom);
}
