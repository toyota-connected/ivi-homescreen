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

#include <memory>
#include <vector>

#include "backend/backend.h"
#include "constants.h"
#include "egl.h"

class WaylandEglBackend : public Egl, public Backend {
 public:
  WaylandEglBackend(struct wl_display* display,
                    bool debug_backend,
                    int buffer_size = kEglBufferSize);

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
  static void Resize(void* user_data,
                     size_t index,
                     Engine* engine,
                     int32_t width,
                     int32_t height);

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
  static void CreateSurface(void* user_data,
                            size_t index,
                            wl_surface* surface,
                            int32_t width,
                            int32_t height);

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

 private:
  wl_egl_window* m_egl_window{};
};
