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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "backend/hud/hud_base.h"

namespace ihs::hud {

// OpenGL-ES render backend for the debug HUD (imgui_impl_opengl3, GLES2). All
// calls must run with the backend's GL context current on the raster thread;
// input is fed from the platform thread via the IHud methods. The HUD renders
// into the default framebuffer (FBO 0) just before the backend's buffer swap.
class GlHud : public HudBase {
 public:
  // Must be called with the GL context current. Returns nullptr with @err set
  // on failure (imgui GL backend init).
  static std::unique_ptr<GlHud> Create(std::string& err);
  ~GlHud() override;

  GlHud(const GlHud&) = delete;
  GlHud& operator=(const GlHud&) = delete;

  // Draw the HUD over the current default framebuffer (@width x @height). Must
  // run with the GL context current, after the frame's own rendering and before
  // the buffer swap.
  void Render(uint32_t width,
              uint32_t height,
              float dt_seconds,
              const HudStats& stats,
              const std::vector<HudViewSample>& views);

  // Render the HUD into an owned offscreen RGBA texture (transparent where the
  // HUD window isn't), returning the GL texture name (0 on failure). For
  // backends that composite the HUD onto a plane buffer rather than the default
  // framebuffer — e.g. the DRM plane compositor, whose scanout buffer is
  // KMS-inverted, so the caller composites this texture with the same y-flip it
  // uses for the app layers. The texture is owned + reused across frames.
  unsigned int RenderOffscreen(uint32_t width,
                               uint32_t height,
                               float dt_seconds,
                               const HudStats& stats,
                               const std::vector<HudViewSample>& views);

 protected:
  void ImplNewFrame() override;
  void ImplRenderDrawData(ImDrawData* draw_data) override;

 private:
  GlHud() = default;

  // Owned offscreen target for RenderOffscreen (lazily sized to the render
  // extent). 0 until first use.
  unsigned int offscreen_fbo_{0};
  unsigned int offscreen_tex_{0};
  uint32_t offscreen_w_{0};
  uint32_t offscreen_h_{0};
};

}  // namespace ihs::hud
