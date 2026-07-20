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

namespace ihs::hud {

// Per-frame compositor stats the HUD window shows. Filled by each backend from
// its own present cadence + capabilities.
struct HudStats {
  float fps{0.0f};
  float frame_ms{0.0f};      // average present interval
  float frame_max_ms{0.0f};  // worst interval in the window
  float target_fps{60.0f};   // display refresh; the green/red threshold
  uint32_t stalls{0};        // presents that blocked on a buffer/flip
  bool dmabuf_present{false};
  bool explicit_sync{false};
};

// One platform view composited this frame (built by a backend from the layer
// list). Identity is the Flutter platform-view id.
struct HudViewSample {
  int64_t id{0};
  uint32_t width{0};
  uint32_t height{0};
  bool dmabuf{false};  // composited via a zero-copy dma-buf path
};

// Appearance/layout of the HUD overlay, sourced from the view's config.toml
// [hud] table (see Configuration). The HUD is a non-interactive overlay —
// enablement is via config/IVI_HUD, not a hot key — so these control only where
// it sits and how its text looks.
struct HudConfig {
  enum class Corner { kTopLeft, kTopRight, kBottomLeft, kBottomRight };
  Corner corner{Corner::kTopLeft};
  float margin{12.0f};     // pixels from the chosen corner
  float font_scale{1.0f};  // multiplies imgui's base font size
  float bg_alpha{0.75f};   // window background opacity
  // Text color, RGBA in [0,1].
  float text_r{1.0f};
  float text_g{1.0f};
  float text_b{1.0f};
  float text_a{1.0f};
};

// Backend-neutral surface of the debug HUD. Rendering is backend-specific (see
// VulkanHud / GlHud); the HUD is a non-interactive overlay enabled via config /
// IVI_HUD (no hot key, no pointer capture). Kept as an interface so backends
// can hold it uniformly.
class IHud {
 public:
  virtual ~IHud() = default;

  [[nodiscard]] virtual bool IsOpen() const = 0;
  virtual void SetOpen(bool open) = 0;
};

}  // namespace ihs::hud
