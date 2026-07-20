/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "backend/hud/gl_hud.h"

#include <GLES2/gl2.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"

namespace ihs::hud {

std::unique_ptr<GlHud> GlHud::Create(std::string& err) {
  std::unique_ptr<GlHud> hud(new GlHud());
  hud->InitContext();
  // GLSL ES 1.00 shaders (GLES2). imgui_impl_opengl3 loads GL entry points via
  // its own bundled loader, so no external GL loader is needed here.
  if (!ImGui_ImplOpenGL3_Init("#version 100")) {
    err = "ImGui_ImplOpenGL3_Init failed";
    return nullptr;
  }
  return hud;
}

GlHud::~GlHud() {
  // Shut the imgui GL backend down while the context is current (the caller
  // guarantees the GL context is current at teardown); the base destructor then
  // frees the imgui context.
  if (imgui_ctx_ != nullptr) {
    MakeCurrent();
    ImGui_ImplOpenGL3_Shutdown();
  }
}

void GlHud::ImplNewFrame() {
  ImGui_ImplOpenGL3_NewFrame();
}

void GlHud::ImplRenderDrawData(ImDrawData* draw_data) {
  ImGui_ImplOpenGL3_RenderDrawData(draw_data);
}

void GlHud::Render(uint32_t width,
                   uint32_t height,
                   float dt_seconds,
                   const HudStats& stats,
                   const std::vector<HudViewSample>& views) {
  // Draw over the window's default framebuffer at full viewport.
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
  RenderFrame(width, height, dt_seconds, stats, views);
}

}  // namespace ihs::hud
