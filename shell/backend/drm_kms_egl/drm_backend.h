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

#include <memory>
#include <string>

#include <shell/platform/embedder/embedder.h>

#include "backend/backend.h"

class DrmCompositor;

struct DrmConfig {
  std::string drm_device;
  uint32_t width;
  uint32_t height;
};

class DrmBackend : public Backend {
 public:
  static std::unique_ptr<DrmBackend> Create(const DrmConfig& cfg);
  ~DrmBackend() override;

  DrmBackend(const DrmBackend&) = delete;
  DrmBackend& operator=(const DrmBackend&) = delete;

  bool MakeCurrent();
  bool ClearCurrent();
  bool Present();
  uint32_t FboCallback(const FlutterFrameInfo* info);
  void* ProcResolver(const char* name);

  FlutterCompositor MakeCompositor();

  [[nodiscard]] uint32_t width() const { return cfg_.width; }
  [[nodiscard]] uint32_t height() const { return cfg_.height; }

  void Resize(size_t /* index */, Engine* /* engine */, int32_t /* w */, int32_t /* h */) override {};
  void CreateSurface(size_t, struct wl_surface*, int32_t, int32_t) override {}
  bool TextureMakeCurrent() override { return false; }
  bool TextureClearCurrent() override { return false; }
  FlutterRendererConfig GetRenderConfig() override { return {}; }
  FlutterCompositor GetCompositorConfig() override { return {}; }
  bool GetEglContext(BackendEglContext* /* out */) const override { return false; }

 private:
  explicit DrmBackend(DrmConfig  cfg);

  DrmConfig cfg_;
};
