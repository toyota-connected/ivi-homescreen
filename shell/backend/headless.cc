/*
 * Copyright 2021-2024 Toyota Connected North America
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

#include "headless.h"
#include "logging.h"
#include "engine.h"
#include "osmesa.h"
#include "gl_process_resolver.h"
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"

struct FlutterDesktopEngineState;

HeadlessBackend::HeadlessBackend(uint32_t initial_width,
                              uint32_t initial_height,
                              const bool debug_backend,
                              const int buffer_size)
    : OSMesaHeadless(initial_width, initial_height),
      Backend(),
      m_prev_width(initial_width),
      m_prev_height(initial_height) {}

void HeadlessBackend::Resize(size_t /* index */,
                             Engine* engine,
                             int32_t width,
                             int32_t height) {
  m_prev_width = m_width;
  m_prev_height = m_height;
  m_width = static_cast<uint32_t>(width);
  m_height = static_cast<uint32_t>(height);
  free_buffer();
  m_buf = create_osmesa_buffer(m_width, m_height);
  MakeCurrent();
  if (engine) {
    auto result = engine->SetWindowSize(static_cast<size_t>(m_height),
                                        static_cast<size_t>(m_width));
    if (result != kSuccess) {
      spdlog::error("Failed to set Flutter Engine Window Size");
    }
  }
}

void HeadlessBackend::CreateSurface(size_t /* index */,
                                      wl_surface* /*surface*/,
                                      int32_t width,
                                      int32_t height) {}

FlutterRendererConfig HeadlessBackend::GetRenderConfig() {
  return {.type = kOpenGL,
          .open_gl = {
              .struct_size = sizeof(FlutterOpenGLRendererConfig),
              .make_current = [](void* user_data) -> bool {
                const auto state =
                    static_cast<FlutterDesktopEngineState*>(user_data);
                return reinterpret_cast<HeadlessBackend*>(
                           state->view_controller->engine->GetBackend())
                    ->MakeCurrent();
              },
              .clear_current = [](void* userdata) -> bool {
                const auto state =
                    static_cast<FlutterDesktopEngineState*>(userdata);
                return reinterpret_cast<HeadlessBackend*>(
                           state->view_controller->engine->GetBackend())
                    ->ClearCurrent();
              },
              .present = [](void* userdata) -> bool {
                const auto state =
                    static_cast<FlutterDesktopEngineState*>(userdata);
                auto* b = reinterpret_cast<HeadlessBackend*>(
                    state->view_controller->engine->GetBackend());
                b->Finish();
                return true;
              },
              .fbo_callback = [](void*) -> uint32_t {
                return 0;  // FBO0
              },
              .make_resource_current = [](void* userdata) -> bool {
                const auto state =
                    static_cast<FlutterDesktopEngineState*>(userdata);
                return reinterpret_cast<HeadlessBackend*>(
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
                *texture_out = {
                    .target = target->target,
                    .name = target->name,
                    .format = target->format,
                    .user_data = target->release_context,
                    .destruction_callback = target->release_callback,
                    .width = target->width,
                    .height = target->height
                };
                target->visible_width = width;
                target->visible_width = height;
                return true;
              },
          }};
}

bool HeadlessBackend::TextureMakeCurrent() {
  return MakeTextureCurrent();
}

bool HeadlessBackend::TextureClearCurrent() {
  return ClearCurrent();
}

FlutterCompositor HeadlessBackend::GetCompositorConfig() {
  return {.struct_size = sizeof(FlutterCompositor),
          .user_data = this,
          .create_backing_store_callback = nullptr,
          .collect_backing_store_callback = nullptr,
          .present_layers_callback = nullptr,
          .avoid_backing_store_cache = true};
}

GLubyte* HeadlessBackend::getHeadlessBuffer()
{
  return m_buf;
}
