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

#include "wayland_drm.h"

#include "engine.h"

WaylandDrmBackend::WaylandDrmBackend(struct wl_display* display,
                                     const uint32_t initial_width,
                                     const uint32_t initial_height,
                                     const bool debug_backend,
                                     const int buffer_size)
    : Backend(),
      m_initial_width(initial_width),
      m_initial_height(initial_height) {}

FlutterRendererConfig WaylandDrmBackend::GetRenderConfig() {
  return {
      .type = kOpenGL,
  };
}

FlutterCompositor WaylandDrmBackend::GetCompositorConfig() {
  return {.struct_size = sizeof(FlutterCompositor),
          .user_data = this,
          .avoid_backing_store_cache = true};
}

WaylandDrmBackend::~WaylandDrmBackend() = default;

void WaylandDrmBackend::Resize(size_t index,
                               Engine* engine,
                               int32_t width,
                               int32_t height) {}

void WaylandDrmBackend::CreateSurface(size_t index,
                                      wl_surface* surface,
                                      int32_t width,
                                      int32_t height) {}

bool WaylandDrmBackend::TextureMakeCurrent() {
  return true;
}

bool WaylandDrmBackend::TextureClearCurrent() {
  return true;
}
