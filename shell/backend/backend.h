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

#include <flutter_embedder.h>

#include "constants.h"
#include "engine.h"

class Engine;

typedef void (*ResizeCallback)(void* /* user_data */,
                               size_t /* index */,
                               Engine* /* flutter engine */,
                               int32_t /* width */,
                               int32_t /* height */);

typedef void (*CreateSurfaceCallback)(void* /* user_data */,
                                      size_t /* index */,
                                      struct wl_surface* /* surface */,
                                      int32_t /* width */,
                                      int32_t /* height */);

class Backend {
 public:
  Backend(void* user_data,
          ResizeCallback resize_callback,
          CreateSurfaceCallback create_surface_callback)
      : m_user_data(user_data),
        m_resize_callback(resize_callback),
        m_create_surface_callback(create_surface_callback) {}

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
  * internal
  */
  void Resize(size_t index,
              const std::shared_ptr<Engine>& flutter_engine,
              int32_t width,
              int32_t height) {
    if (m_resize_callback) {
      m_resize_callback(m_user_data, index, flutter_engine.get(), width,
                        height);
    }
  }

  /**
  * @brief Execute the callback function for surface creating
  * @param[in] index Set Application ID
  * @param[in] surface Pointer to surface
  * @param[in] width Set surface width
  * @param[in] height Set surface height
  * @return void
  * @relation
  * internal
  */
  void CreateSurface(size_t index,
                     struct wl_surface* surface,
                     int32_t width,
                     int32_t height) {
    if (m_create_surface_callback) {
      m_create_surface_callback(m_user_data, index, surface, width, height);
    }
  }

  /**
  * @brief Get an empty FlutterRendererConfig
  * @return FlutterRendererConfig
  * @retval Pointer to FlutterRendererConfig
  * @relation
  * internal
  */
  virtual FlutterRendererConfig GetRenderConfig() { return {}; }

  /**
  * @brief Get an empty FlutterCompositor
  * @return FlutterCompositor
  * @retval Pointer to FlutterCompositor
  * @relation
  * internal
  */
  MAYBE_UNUSED virtual FlutterCompositor GetCompositorConfig() { return {}; }

 private:
  void* m_user_data;
  ResizeCallback m_resize_callback;
  CreateSurfaceCallback m_create_surface_callback;
};
