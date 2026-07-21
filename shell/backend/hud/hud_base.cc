/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "backend/hud/hud_base.h"

#include <algorithm>

#include "imgui.h"

namespace ihs::hud {

namespace {
ImGuiContext* Ctx(void* p) {
  return static_cast<ImGuiContext*>(p);
}
}  // namespace

HudBase::~HudBase() {
  // The concrete backend has already shut its imgui render backend down (in its
  // own destructor, while the device/GL context was still valid); only the
  // context itself is left to free here.
  if (imgui_ctx_ != nullptr) {
    ImGui::DestroyContext(Ctx(imgui_ctx_));
    imgui_ctx_ = nullptr;
  }
}

bool HudBase::InitContext() {
  IMGUI_CHECKVERSION();
  imgui_ctx_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(Ctx(imgui_ctx_));
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // don't write imgui.ini from a compositor
  io.LogFilename = nullptr;
  ImGui::StyleColorsDark();
  return true;
}

void HudBase::MakeCurrent() const {
  if (imgui_ctx_ != nullptr) {
    ImGui::SetCurrentContext(Ctx(imgui_ctx_));
  }
}

void HudBase::UpdateViewRows(const std::vector<HudViewSample>& views) {
  ++hud_frame_;
  for (const auto& v : views) {
    auto [it, inserted] = view_rows_.try_emplace(v.id);
    ViewRow& row = it->second;
    if (inserted) {
      row.first_seen_frame = hud_frame_;
    }
    row.width = v.width;
    row.height = v.height;
    row.dmabuf = v.dmabuf;
    row.alive = true;
    row.last_seen_frame = hud_frame_;
    ++row.presents;
  }
  // Age out rows: a view not composited for a while is treated as disposed and,
  // after a short grace period (so the dispose flashes), dropped.
  constexpr uint64_t kDeadAfter = 4;    // frames without a present -> disposed
  constexpr uint64_t kDropAfter = 120;  // frames after that -> forget the row
  for (auto it = view_rows_.begin(); it != view_rows_.end();) {
    const uint64_t idle = hud_frame_ - it->second.last_seen_frame;
    if (idle >= kDeadAfter) {
      it->second.alive = false;
    }
    if (idle >= kDropAfter) {
      it = view_rows_.erase(it);
    } else {
      ++it;
    }
  }
}

void HudBase::DrawFpsHistogram(float target_fps) {
  const float target = target_fps > 1.0f ? target_fps : 60.0f;
  // A frame counts as "good" (green) at/above 90% of the display refresh; below
  // that it is dropping frames, so draw it red.
  const float good_fps = target * 0.9f;
  // Full-height bar at 1.1x refresh so an at-target frame nearly fills.
  const float scale_fps = target * 1.1f;

  const auto n = static_cast<int>(frame_ms_history_.size());
  const float avail = ImGui::GetContentRegionAvail().x;
  const ImVec2 size(avail > 0.0f ? avail : 320.0f, 46.0f);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::Dummy(size);  // reserve the layout box

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y),
                    IM_COL32(20, 20, 20, 160));
  // Reference line at the good/green threshold.
  const float thresh_y =
      p0.y + size.y * (1.0f - std::clamp(good_fps / scale_fps, 0.0f, 1.0f));
  dl->AddLine(ImVec2(p0.x, thresh_y), ImVec2(p0.x + size.x, thresh_y),
              IM_COL32(90, 90, 90, 200));

  const float bar_w = size.x / static_cast<float>(n);
  for (int i = 0; i < n; ++i) {
    // Walk oldest -> newest so the chart scrolls left to right.
    const int idx = (frame_ms_head_ + i) % n;
    const float ms = frame_ms_history_[static_cast<size_t>(idx)];
    if (ms <= 0.0f) {
      continue;
    }
    const float fps = 1000.0f / ms;
    const float h = size.y * std::clamp(fps / scale_fps, 0.0f, 1.0f);
    const float x0 = p0.x + static_cast<float>(i) * bar_w;
    const ImU32 col = fps >= good_fps ? IM_COL32(60, 200, 70, 220)
                                      : IM_COL32(210, 60, 55, 220);
    dl->AddRectFilled(ImVec2(x0, p0.y + size.y - h),
                      ImVec2(x0 + bar_w - 1.0f, p0.y + size.y), col);
  }
}

void HudBase::BuildWindow(const HudStats& stats,
                          const std::vector<HudViewSample>& views) {
  frame_ms_history_[static_cast<size_t>(frame_ms_head_)] = stats.frame_ms;
  frame_ms_head_ =
      (frame_ms_head_ + 1) % static_cast<int>(frame_ms_history_.size());

  // Position by configured corner. DisplaySize is set in RenderFrame; anchor
  // the window's matching corner (pivot) so it hugs the chosen edge with a
  // margin.
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  const float m = config_.margin;
  ImVec2 pos;
  ImVec2 pivot;
  switch (config_.corner) {
    case HudConfig::Corner::kTopRight:
      pos = ImVec2(disp.x - m, m);
      pivot = ImVec2(1.0f, 0.0f);
      break;
    case HudConfig::Corner::kBottomLeft:
      pos = ImVec2(m, disp.y - m);
      pivot = ImVec2(0.0f, 1.0f);
      break;
    case HudConfig::Corner::kBottomRight:
      pos = ImVec2(disp.x - m, disp.y - m);
      pivot = ImVec2(1.0f, 1.0f);
      break;
    case HudConfig::Corner::kTopLeft:
    default:
      pos = ImVec2(m, m);
      pivot = ImVec2(0.0f, 0.0f);
      break;
  }
  ImGui::SetNextWindowBgAlpha(config_.bg_alpha);
  ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_Always);
  ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);

  // Non-interactive overlay: no title bar / close box (enablement is via config
  // / IVI_HUD), no nav/focus stealing, and locked in place.
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoInputs;
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(config_.text_r, config_.text_g,
                                              config_.text_b, config_.text_a));
  if (ImGui::Begin("Compositor HUD", nullptr, flags)) {
    ImGui::SetWindowFontScale(config_.font_scale);
    ImGui::Text("FPS        %6.1f  / target %.0f",
                static_cast<double>(stats.fps),
                static_cast<double>(stats.target_fps));
    ImGui::Text("frame      %6.2f ms", static_cast<double>(stats.frame_ms));
    ImGui::Text("worst      %6.2f ms", static_cast<double>(stats.frame_max_ms));
    ImGui::Text("stalls     %6u", stats.stalls);
    DrawFpsHistogram(stats.target_fps);
    ImGui::Separator();
    ImGui::Text("dma-buf present : %s",
                stats.dmabuf_present ? "on" : "off (WSI)");
    ImGui::Text("explicit sync   : %s", stats.explicit_sync ? "on" : "off");

    ImGui::Separator();
    ImGui::Text("platform views : %zu this frame / %zu tracked", views.size(),
                view_rows_.size());
    if (ImGui::BeginTable("pv", 5,
                          ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_BordersInnerH)) {
      ImGui::TableSetupColumn("id");
      ImGui::TableSetupColumn("size");
      ImGui::TableSetupColumn("presents");
      ImGui::TableSetupColumn("Δf");
      ImGui::TableSetupColumn("path");
      ImGui::TableHeadersRow();
      for (const auto& [id, row] : view_rows_) {
        ImGui::TableNextRow();
        // Flash green just after a view appears, red while it is dying.
        const uint64_t age = hud_frame_ - row.first_seen_frame;
        const uint64_t idle = hud_frame_ - row.last_seen_frame;
        if (!row.alive) {
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                 IM_COL32(120, 30, 30, 160));
        } else if (age < 30) {
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                 IM_COL32(30, 110, 30, 160));
        }
        ImGui::TableNextColumn();
        ImGui::Text("%lld", static_cast<long long>(id));
        ImGui::TableNextColumn();
        ImGui::Text("%ux%u", row.width, row.height);
        ImGui::TableNextColumn();
        ImGui::Text("%llu", static_cast<unsigned long long>(row.presents));
        ImGui::TableNextColumn();
        ImGui::Text("%llu", static_cast<unsigned long long>(idle));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(!row.alive ? "disposed"
                                          : (row.dmabuf ? "dma-buf" : "WSI"));
      }
      ImGui::EndTable();
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
}

void HudBase::RenderFrame(uint32_t width,
                          uint32_t height,
                          float dt_seconds,
                          const HudStats& stats,
                          const std::vector<HudViewSample>& views) {
  if (imgui_ctx_ == nullptr) {
    return;
  }
  ImGui::SetCurrentContext(Ctx(imgui_ctx_));
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize =
      ImVec2(static_cast<float>(width), static_cast<float>(height));
  io.DeltaTime = dt_seconds > 0.0f ? dt_seconds : 1.0f / 60.0f;

  UpdateViewRows(views);

  ImplNewFrame();
  ImGui::NewFrame();
  if (open_) {
    BuildWindow(stats, views);
  }
  ImGui::Render();

  ImDrawData* draw_data = ImGui::GetDrawData();
  if (draw_data == nullptr || draw_data->TotalVtxCount == 0) {
    return;  // window closed / empty — nothing to record
  }
  ImplRenderDrawData(draw_data);
}

}  // namespace ihs::hud
