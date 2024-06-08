/*
 * Copyright 2022 Toyota Connected North America
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

#include "config.h"

#include <flutter_texture_registrar.h>
#include <shell/platform/embedder/embedder.h>


class Engine;

class Backend {
 public:
  enum Type {
    Headless,
    WaylandEgl,
    WaylandVulkan,
    WaylandLeasedDrm,
    DrmKms,
  };

  Backend() = default;
  virtual ~Backend() = default;

  Backend(const Backend&) = delete;
  const Backend& operator=(const Backend&) = delete;

  /**
   * @brief Execute the callback function for window resizing
   * @param[in] index Set Application ID
   * @param[in] flutter_engine Pointer to Flutter engine
   * @param[in] width Set window width
   * @param[in] height Set window height
   * @return void
   * @relation
   * flutter, wayland
   */
  virtual void Resize(size_t index,
                      Engine* flutter_engine,
                      int32_t width,
                      int32_t height) = 0;

  /**
   * @brief Execute the callback function for surface creating
   * @param[in] index Set Application ID
   * @param[in] surface Pointer to surface
   * @param[in] width Set surface width
   * @param[in] height Set surface height
   * @return void
   * @relation
   * wayland, (flutter)
   */
  virtual void CreateSurface(size_t index,
                             struct wl_surface* surface,
                             int32_t width,
                             int32_t height) = 0;

  virtual bool TextureMakeCurrent() = 0;

  virtual bool TextureClearCurrent() = 0;

  /**
   * @brief Get an empty FlutterRendererConfig
   * @return FlutterRendererConfig
   * @retval Pointer to FlutterRendererConfig
   * @relation
   * internal
   */
  virtual FlutterRendererConfig GetRenderConfig() = 0;

  /**
   * @brief Get an empty FlutterCompositor
   * @return FlutterCompositor
   * @retval Pointer to FlutterCompositor
   * @relation
   * internal
   */
  virtual FlutterCompositor GetCompositorConfig() = 0;
};
