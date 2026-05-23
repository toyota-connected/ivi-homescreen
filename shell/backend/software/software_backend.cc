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
#include <ctime>
#include <string_view>
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

VsyncCallback SoftwareBackend::GetVsyncCallback() const {
  static const bool env_disabled = []() {
    const char* env = std::getenv("IVI_SW_VSYNC");
    return env != nullptr && std::string_view(env) == "0";
  }();
  if (env_disabled) {
    static const bool logged = []() {
      spdlog::info("[SoftwareBackend] IVI_SW_VSYNC=0 — wall-clock scheduler");
      return true;
    }();
    (void)logged;
    return nullptr;
  }
  return (sink_ && sink_->SupportsVsync()) ? &VsyncTrampoline : nullptr;
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
  backend->ProfilePresent(ok);
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

void SoftwareBackend::VsyncTrampoline(void* user_data, const intptr_t baton) {
  auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  if (state == nullptr || state->view_controller == nullptr ||
      state->view_controller->engine == nullptr) {
    return;
  }
  auto* engine_obj = state->view_controller->engine;
  auto* backend = dynamic_cast<SoftwareBackend*>(engine_obj->GetBackend());
  if (backend == nullptr || backend->sink_ == nullptr) {
    return;
  }
  backend->sink_->SubmitBaton(
      static_cast<void*>(engine_obj->GetFlutterEngine()), baton);
}

SoftwareBackend::~SoftwareBackend() {
  // Session-aggregate profile summary (IVI_SW_PROFILE=1). Logged from
  // the dtor so it captures the whole run regardless of which sink
  // was active. No-op when the env var was unset (counters never
  // accumulated).
  const auto& s = session_totals_;
  if (s.presented_frames > 0) {
    const uint32_t samples =
        (s.interval_sum_ns > 0) ? s.presented_frames - 1 : s.presented_frames;
    const uint64_t mean_ns = samples > 0 ? s.interval_sum_ns / samples : 0;
    const double fps = mean_ns > 0 ? 1e9 / static_cast<double>(mean_ns) : 0.0;
    const uint32_t total = s.bucket_60hz + s.bucket_30hz + s.bucket_20hz +
                           s.bucket_slow + s.bucket_idle;
    spdlog::info(
        "[SoftwareBackend] session summary: frames={} fps={:.2f} "
        "mean_interval={}us max_interval={}us present_failures={}",
        s.presented_frames, fps, mean_ns / 1000, s.interval_max_ns / 1000,
        s.present_failures);
    if (total > 0) {
      const double inv = 100.0 / static_cast<double>(total);
      spdlog::info(
          "[SoftwareBackend] session buckets: "
          "60Hz(≤17ms)={} ({:.1f}%) 30Hz(18-33ms)={} ({:.1f}%) "
          "20Hz(34-50ms)={} ({:.1f}%) slow(51-100ms)={} ({:.1f}%) "
          "idle(>100ms)={} ({:.1f}%)",
          s.bucket_60hz, s.bucket_60hz * inv, s.bucket_30hz,
          s.bucket_30hz * inv, s.bucket_20hz, s.bucket_20hz * inv,
          s.bucket_slow, s.bucket_slow * inv, s.bucket_idle,
          s.bucket_idle * inv);
    }
  }
}

void SoftwareBackend::ProfilePresent(const bool ok) {
  // Single env-var probe per process; the lookup is O(strlen) and we
  // call this on every frame.
  static const bool profile_enabled = std::getenv("IVI_SW_PROFILE") != nullptr;
  if (!profile_enabled) {
    return;
  }
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const uint64_t now_ns = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
                          static_cast<uint64_t>(ts.tv_nsec);

  auto& p = profile_;
  if (!ok) {
    ++p.present_failures;
    return;
  }
  if (p.last_present_ns != 0) {
    const uint64_t dt = now_ns - p.last_present_ns;
    p.interval_sum_ns += dt;
    if (dt > p.interval_max_ns) {
      p.interval_max_ns = dt;
    }
    // Same bucket thresholds as wayland_egl / wayland_vulkan so
    // histograms line up across backends.
    if (dt <= 17'000'000ULL) {
      ++p.bucket_60hz;
    } else if (dt <= 33'000'000ULL) {
      ++p.bucket_30hz;
    } else if (dt <= 50'000'000ULL) {
      ++p.bucket_20hz;
    } else if (dt <= 100'000'000ULL) {
      ++p.bucket_slow;
    } else {
      ++p.bucket_idle;
    }
  }
  p.last_present_ns = now_ns;
  ++p.presented_frames;

  constexpr uint32_t kProfileWindow = 60;
  if (p.presented_frames < kProfileWindow) {
    return;
  }
  const uint32_t samples =
      (p.interval_sum_ns > 0) ? p.presented_frames - 1 : p.presented_frames;
  const uint64_t mean_ns = samples > 0 ? p.interval_sum_ns / samples : 0;
  const double fps = mean_ns > 0 ? 1e9 / static_cast<double>(mean_ns) : 0.0;
  spdlog::info(
      "[SoftwareBackend] profile (n={}): fps={:.2f} mean_interval={}us "
      "max_interval={}us present_failures={} "
      "buckets[60Hz/30Hz/20Hz/slow/idle]={}/{}/{}/{}/{}",
      p.presented_frames, fps, mean_ns / 1000, p.interval_max_ns / 1000,
      p.present_failures, p.bucket_60hz, p.bucket_30hz, p.bucket_20hz,
      p.bucket_slow, p.bucket_idle);

  auto& s = session_totals_;
  s.presented_frames += p.presented_frames;
  s.present_failures += p.present_failures;
  s.interval_sum_ns += p.interval_sum_ns;
  if (p.interval_max_ns > s.interval_max_ns) {
    s.interval_max_ns = p.interval_max_ns;
  }
  s.bucket_60hz += p.bucket_60hz;
  s.bucket_30hz += p.bucket_30hz;
  s.bucket_20hz += p.bucket_20hz;
  s.bucket_slow += p.bucket_slow;
  s.bucket_idle += p.bucket_idle;
  p = FrameProfile{.last_present_ns = p.last_present_ns};
}
