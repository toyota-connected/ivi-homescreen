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

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "backend/backend.h"

namespace homescreen {
class DrmSession;
}  // namespace homescreen

// Phase 0 scaffold for the DRM/KMS Vulkan scanout backend (drm_kms_vulkan
// plan). The backend drives Flutter's Vulkan renderer and scans the result
// out on hardware KMS planes via zero-copy dma-buf import, reusing the
// session / modeset / vsync stack from drm_kms_egl below the pixel layer.
//
// Today only the scaffold exists: Create() runs the §4.1 zero-copy gate and
// then refuses (returns nullptr) in every case — either because the gate
// failed (and logs the precise cause) or because the render/scanout pipeline
// is not implemented yet (Phases 1-6). The class implements the Backend
// interface so the FlutterView wiring, the Backend::Type enumerator, and the
// build graph are all in place ahead of the pixel path.
class VulkanDrmBackend final : public Backend {
 public:
  // Probe the §4.1 gate and (for now) refuse. Returns nullptr on every path:
  // the caller treats null as a hard init failure and aborts, exactly as the
  // drm_kms_egl backend does on DrmBackend::Create returning null. @p session
  // may be null when no libseat session is available.
  static std::shared_ptr<VulkanDrmBackend> Create(
      const std::string& drm_device,
      bool enable_validation,
      homescreen::DrmSession* session);

  ~VulkanDrmBackend() override = default;

  // ── Backend interface ──────────────────────────────────────────────────────
  // Stubs until the render/scanout path lands; never reached while Create()
  // refuses, but required for the vtable and the FlutterView call sites.
  void Resize(size_t index,
              Engine* flutter_engine,
              int32_t width,
              int32_t height) override;
  void CreateSurface(size_t index,
                     struct wl_surface* surface,
                     int32_t width,
                     int32_t height) override;
  bool TextureMakeCurrent() override;
  bool TextureClearCurrent() override;
  FlutterRendererConfig GetRenderConfig() override;
  FlutterCompositor GetCompositorConfig() override;

  // Resolved framebuffer dimensions, queried by FlutterView for the display
  // metrics event (the DRM path has no WaylandWindow to source them).
  [[nodiscard]] uint32_t width() const { return width_; }
  [[nodiscard]] uint32_t height() const { return height_; }

 private:
  VulkanDrmBackend(uint32_t width,
                   uint32_t height,
                   homescreen::DrmSession* session);

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  homescreen::DrmSession* session_ = nullptr;  // not owned
};
