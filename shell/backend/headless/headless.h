/*
 * Copyright 2021-2024 Toyota Connected North America
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

#include <GL/osmesa.h>

#include "config.h"

#include "../backend.h"
#include "osmesa.h"

class Backend;

class Engine;

class HeadlessBackend : public OSMesaHeadless, public Backend {
 public:
  HeadlessBackend(uint32_t initial_width,
                  uint32_t initial_height,
                  bool debug_backend,
                  int buffer_size);

  /**
   * @brief Resize Flutter engine Window size
   * @param[in] user_data Pointer to User data
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
   * @param[in] user_data Pointer to User data
   * @param[in] index No use
   * @param[in] surface Pointer to surface
   * @param[in] width Set surface width
   * @param[in] height Set surface height
   * @return void
   * @relation
   * wayland
   */
  void CreateSurface(size_t index,
                     struct wl_surface* surface,
                     int32_t width,
                     int32_t height) override;

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

  GLubyte* getHeadlessBuffer();

 private:
  uint32_t m_prev_width, m_width;
  uint32_t m_prev_height, m_height;
};
