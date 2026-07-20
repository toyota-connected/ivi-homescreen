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
  if (offscreen_tex_ != 0) {
    glDeleteTextures(1, &offscreen_tex_);
  }
  if (offscreen_fbo_ != 0) {
    glDeleteFramebuffers(1, &offscreen_fbo_);
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

unsigned int GlHud::RenderOffscreen(uint32_t width,
                                    uint32_t height,
                                    float dt_seconds,
                                    const HudStats& stats,
                                    const std::vector<HudViewSample>& views) {
  if (width == 0 || height == 0) {
    return 0;
  }
  // (Re)allocate the owned RGBA target if the extent changed.
  if (offscreen_fbo_ == 0 || offscreen_w_ != width || offscreen_h_ != height) {
    if (offscreen_tex_ != 0) {
      glDeleteTextures(1, &offscreen_tex_);
    }
    if (offscreen_fbo_ != 0) {
      glDeleteFramebuffers(1, &offscreen_fbo_);
    }
    glGenTextures(1, &offscreen_tex_);
    glBindTexture(GL_TEXTURE_2D, offscreen_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
                 static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &offscreen_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, offscreen_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           offscreen_tex_, 0);
    offscreen_w_ = width;
    offscreen_h_ = height;
  } else {
    glBindFramebuffer(GL_FRAMEBUFFER, offscreen_fbo_);
  }

  glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f,
               0.0f);  // transparent — only the HUD composites
  glClear(GL_COLOR_BUFFER_BIT);
  RenderFrame(width, height, dt_seconds, stats, views);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return offscreen_tex_;
}

}  // namespace ihs::hud
