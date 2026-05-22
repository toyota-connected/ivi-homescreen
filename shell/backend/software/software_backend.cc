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

#include <csignal>
#include <cstdlib>
#include <cstring>
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
  if (const char* env = std::getenv("IVI_SW_STOP_AFTER_FRAMES");
      env != nullptr && env[0] != '\0') {
    char* end = nullptr;
    // strtoull silently wraps a leading '-' into a huge positive value
    // (e.g. "-1" → ULLONG_MAX), which would defeat the "positive integer"
    // gate below. Reject leading sign characters explicitly so "-1" or
    // "+0" produce a warn instead of an absurd stop-after threshold.
    const bool has_sign = env[0] == '-' || env[0] == '+';
    const unsigned long long n = has_sign ? 0ULL : std::strtoull(env, &end, 10);
    if (!has_sign && end != env && *end == '\0' && n > 0) {
      stop_after_frames_ = n;
      spdlog::info("[SoftwareBackend] stop_after_frames={}", n);
    } else {
      spdlog::warn(
          "[SoftwareBackend] IVI_SW_STOP_AFTER_FRAMES='{}' not a "
          "positive integer; ignored",
          env);
    }
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
  const bool ok = backend->sink_->Present(allocation, row_bytes, height);
  if (ok && backend->stop_after_frames_ > 0) {
    const uint64_t presented = ++backend->presented_frames_;
    if (presented >= backend->stop_after_frames_) {
      // First crosser fires SIGTERM; subsequent presents are harmless.
      // Raising the signal lets the existing main.cc handler exit
      // cleanly instead of forcing a hard exit() from a rasterizer-
      // thread context. std::raise is async-signal-safe and main.cc's
      // handler only flips a sig_atomic_t flag, so the rasterizer
      // thread unwinds the trampoline normally before App::Loop
      // observes the flag and returns.
      if (bool expected = false;
          backend->stop_signaled_.compare_exchange_strong(expected, true)) {
        spdlog::info(
            "[SoftwareBackend] reached stop_after_frames={}; raising SIGTERM",
            backend->stop_after_frames_);
        std::raise(SIGTERM);
      }
    }
  }
  return ok;
}
