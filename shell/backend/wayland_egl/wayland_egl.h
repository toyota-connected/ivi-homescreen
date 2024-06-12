/*
 * Copyright 2020-2022 Toyota Connected North America
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

#include <list>
#include <unordered_map>

#include <wayland-egl.h>

#include "config/common.h"

#include "backend/backend.h"
#include "egl.h"

class Backend;

class Engine;

class WaylandEglBackend : public Egl, public Backend {
 public:
  // Maximum damage history - for triple buffering we need to store damage for
  // last two frames.
  static constexpr int kMaxHistorySize = 10;

  WaylandEglBackend(struct wl_display* display,
                    uint32_t initial_width,
                    uint32_t initial_height,
                    bool debug_backend,
                    int buffer_size = kEglBufferSize);

  /**
   * @brief Resize Flutter engine Window size
   * @param[in] index No use
   * @param[in] engine Pointer to Flutter engine
   * @param[in] width Set window width
   * @param[in] height Set window height
   * @return void
   * @relation
   * wayland
   */
  void Resize(size_t index,
              Engine* flutter_engine,
              int32_t width,
              int32_t height) override;

  /**
   * @brief Create EGL surface
   * @param[in] index No use
   * @param[in] surface Pointer to surface
   * @param[in] width Set surface width
   * @param[in] height Set surface height
   * @return void
   * @relation
   * wayland
   */
  void CreateSurface(const size_t index,
                     struct wl_surface* surface,
                     const int32_t width,
                     const int32_t height) override;

  bool TextureMakeCurrent() override;

  bool TextureClearCurrent() override;

  /**
   * @brief Get FlutterRendererConfig
   * @return FlutterRendererConfig
   * @retval Pointer to FlutterRendererConfig
   * @relation
   * wayland
   */
  FlutterRendererConfig GetRenderConfig() override;

  /**
   * @brief Get FlutterCompositor
   * @return FlutterCompositor
   * @retval Pointer to FlutterCompositor
   * @relation
   * wayland
   */
  FlutterCompositor GetCompositorConfig() override;

  void UpdateSize(int _width, int _height) {
    m_initial_width = static_cast<uint32_t>(_width);
    m_initial_height = static_cast<uint32_t>(_height);
  }

 private:
  struct wl_egl_window* m_egl_window{};
  uint32_t m_initial_width;
  uint32_t m_initial_height;

  // Keeps track of the existing damage associated with each FBO ID
  std::unordered_map<intptr_t, FlutterRect*> m_existing_damage_map;

  // Keeps track of the most recent frame damages so that existing damage can
  // be easily computed.
  std::list<FlutterRect> m_damage_history{};

  /**
   * @brief Auxiliary function to union the damage regions comprised by two
   * FlutterRect element. It saves the result of this join in the rect variable.
   * @return void
   * @relation
   * wayland
   */
  static void JoinFlutterRect(FlutterRect* rect,
                              const FlutterRect& additional_rect);
};
