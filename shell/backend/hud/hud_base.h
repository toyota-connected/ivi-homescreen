/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "backend/hud/ihud.h"

struct ImDrawData;

namespace ihs::hud {

// Shared, backend-neutral HUD implementation: owns the imgui context, the input
// event forwarding, per-view tracking, the frame-time history + green/red
// histogram, and the window layout. Concrete backends (VulkanHud, GlHud) supply
// only the two impl hooks (new-frame + render-draw-data) and their own Create/
// destroy of the imgui render backend.
//
// Threading: input methods run on the platform thread (imgui queues them);
// RenderFrame + WantCapture* run on the raster/present thread. The concrete
// backend is responsible for not racing the two (both are cheap and the shared
// queue/GL context serialises the GPU work).
class HudBase : public IHud {
 public:
  ~HudBase() override;

  [[nodiscard]] bool IsOpen() const override { return open_; }
  void SetOpen(bool open) override { open_ = open; }

  // Appearance/layout (config.toml [hud]); applied each frame in the window.
  void SetConfig(const HudConfig& config) { config_ = config; }

 protected:
  HudBase() = default;

  // Create the imgui context + style (call once from the concrete Create before
  // initialising the render backend). Returns false only on version mismatch.
  bool InitContext();
  void MakeCurrent() const;

  // Template method: impl new-frame -> build the window -> imgui render -> impl
  // draw. The concrete Render(...) sets up whatever GPU/target state it needs,
  // then calls this. Nothing is recorded when the draw data is empty.
  void RenderFrame(uint32_t width,
                   uint32_t height,
                   float dt_seconds,
                   const HudStats& stats,
                   const std::vector<HudViewSample>& views);

  // Impl hooks (backend-specific imgui render backend).
  virtual void ImplNewFrame() = 0;
  virtual void ImplRenderDrawData(ImDrawData* draw_data) = 0;

  void* imgui_ctx_{nullptr};  // ImGuiContext*

 private:
  void UpdateViewRows(const std::vector<HudViewSample>& views);
  void BuildWindow(const HudStats& stats,
                   const std::vector<HudViewSample>& views);
  void DrawFpsHistogram(float target_fps);

  // Per-view row tracked across frames (keyed by view id).
  struct ViewRow {
    uint32_t width{0};
    uint32_t height{0};
    uint64_t presents{0};
    uint64_t first_seen_frame{0};
    uint64_t last_seen_frame{0};
    bool dmabuf{false};
    bool alive{true};
  };

  bool open_{false};
  HudConfig config_{};
  std::array<float, 120> frame_ms_history_{};
  int frame_ms_head_{0};
  std::unordered_map<int64_t, ViewRow> view_rows_;
  uint64_t hud_frame_{0};
};

}  // namespace ihs::hud
