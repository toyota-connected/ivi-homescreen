/*
 * Copyright 2026 Toyota Connected North America
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

#include "backend/software/software_backend.h"

#include <utility>

#include "engine.h"
#include "logging.h"
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"

SoftwareBackend::SoftwareBackend(const uint32_t initial_width,
                                 const uint32_t initial_height,
                                 std::unique_ptr<ISurfaceSink> sink)
    : Backend(),
      width_(initial_width),
      height_(initial_height),
      sink_(std::move(sink)) {
  if (sink_) {
    sink_->OnSize(width_, height_);
  }
}

void SoftwareBackend::Resize(size_t /* index */,
                             Engine* flutter_engine,
                             const int32_t width,
                             const int32_t height) {
  width_ = static_cast<uint32_t>(width);
  height_ = static_cast<uint32_t>(height);
  if (sink_) {
    sink_->OnSize(width_, height_);
  }
  if (flutter_engine) {
    if (const auto result = flutter_engine->SetWindowSize(
            static_cast<size_t>(height), static_cast<size_t>(width));
        result != kSuccess) {
      spdlog::error(
          "[SoftwareBackend] Failed to set Flutter Engine Window Size");
    }
  }
}

void SoftwareBackend::CreateSurface(size_t /* index */,
                                    wl_surface* /* unused */,
                                    const int32_t width,
                                    const int32_t height) {
  width_ = static_cast<uint32_t>(width);
  height_ = static_cast<uint32_t>(height);
  if (sink_) {
    sink_->OnSize(width_, height_);
  }
}

FlutterRendererConfig SoftwareBackend::GetRenderConfig() {
  FlutterRendererConfig config{};
  config.type = kSoftware;
  config.software.struct_size = sizeof(FlutterSoftwareRendererConfig);
  config.software.surface_present_callback =
      &SoftwareBackend::PresentTrampoline;
  return config;
}

FlutterCompositor SoftwareBackend::GetCompositorConfig() {
  // Compositor disabled. With all callbacks null the engine falls back
  // to surface_present_callback for the entire scene.
  FlutterCompositor compositor{};
  compositor.struct_size = sizeof(FlutterCompositor);
  compositor.user_data = this;
  compositor.create_backing_store_callback = nullptr;
  compositor.collect_backing_store_callback = nullptr;
  compositor.present_layers_callback = nullptr;
  compositor.avoid_backing_store_cache = true;
  compositor.present_view_callback = nullptr;
  return compositor;
}

bool SoftwareBackend::PresentTrampoline(void* user_data,
                                        const void* allocation,
                                        const size_t row_bytes,
                                        const size_t height) {
  auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  if (state == nullptr || state->view_controller == nullptr ||
      state->view_controller->engine == nullptr) {
    return false;
  }
  auto* backend = dynamic_cast<SoftwareBackend*>(
      state->view_controller->engine->GetBackend());
  if (backend == nullptr || backend->sink_ == nullptr) {
    return false;
  }
  return backend->sink_->Present(allocation, row_bytes, height);
}
