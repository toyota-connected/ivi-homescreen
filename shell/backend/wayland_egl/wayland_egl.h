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

#if BUILD_COMPOSITOR
#include <memory>
#include <mutex>

#include "backend/backing_store_pool.h"
#include "backend/wayland_egl/egl_backing_store.h"
#include "backend/wayland_egl/gl_caps.h"
#include "backend/wayland_egl/gl_compositor.h"
#include "view/compositor_surface_interface.h"
#include "view/mutation_stack.h"
#include "view/present_layer_sequencer.h"
#endif

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

#if BUILD_COMPOSITOR
  ~WaylandEglBackend() override;
#endif

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

  bool GetEglContext(BackendEglContext* out) const override;

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

#if BUILD_COMPOSITOR
  /**
   * @brief Register a platform-view surface for composition.
   *
   * Called by @c FlutterView in response to the @c flutter/platform_views
   * create message. Must be paired with @c UnregisterCompositorSurface.
   */
  void RegisterCompositorSurface(
      FlutterPlatformViewIdentifier id,
      std::shared_ptr<ICompositorSurface> surface) override;

  void UnregisterCompositorSurface(FlutterPlatformViewIdentifier id) override;

  void ResizeCompositorSurface(FlutterPlatformViewIdentifier id,
                               int32_t width,
                               int32_t height) override;
#endif

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

#if BUILD_COMPOSITOR
  GlCaps m_gl_caps;
  bool m_gl_caps_probed{false};
  std::unique_ptr<GlCompositor> m_gl_compositor;
  BackingStorePool<EglFboBackingStore> m_fbo_pool;
  BackingStorePool<EglTextureBackingStore> m_texture_pool;
  PresentLayerSequencer m_sequencer;
  // Register/Unregister fire on the platform thread via FlutterView;
  // PresentLayers reads the map on the rasterizer thread. The mutex is
  // held only across map lookups — never across the OnPresent call
  // itself. We snapshot a shared_ptr copy under the lock before dropping
  // it, so OnPresent runs lock-free on a per-frame-stable pointer.
  mutable std::mutex m_compositor_surfaces_mu_;
  std::unordered_map<FlutterPlatformViewIdentifier,
                     std::shared_ptr<ICompositorSurface>>
      m_compositor_surfaces;

  // Holds shared ownership of backing stores while the engine has them
  // checked out. Keyed by raw pointer — the engine's @c user_data baton
  // refers back to this map for Collect.
  std::unordered_map<EglFboBackingStore*, std::shared_ptr<EglFboBackingStore>>
      m_alive_stores_;
  std::unordered_map<EglTextureBackingStore*,
                     std::shared_ptr<EglTextureBackingStore>>
      m_alive_texture_stores_;

  // One-per-backend scratch FBO used to wrap texture-subtype backing
  // stores just before compositing so the blit path in GlCompositor
  // works uniformly. Created lazily.
  GLuint m_texture_blit_fbo_{0};

  /// Lazily probe GL caps and instantiate @c GlCompositor. Must be called
  /// with the EGL context current.
  void EnsureGlCapsProbed();

  bool CreateBackingStore(const FlutterBackingStoreConfig* config,
                          FlutterBackingStore* store_out);
  bool CollectBackingStore(const FlutterBackingStore* store);
  bool PresentLayers(const FlutterLayer** layers, size_t count);

  /// Blit backing FBO into FBO 0 (window) and swap.
  bool BlitBackingStoreToWindow(const FlutterBackingStore* store);

  /// Composite a single layer's backing store onto FBO 0 at the given
  /// pixel rect. Handles both FBO and texture subtypes.
  void CompositeLayer(const FlutterBackingStore* store,
                      GLint dst_x,
                      GLint dst_y,
                      GLsizei dst_w,
                      GLsizei dst_h);

  /// True when the requested size equals the view's root dimensions.
  /// Used to pick FBO vs texture subtype at create time.
  [[nodiscard]] bool IsRootSize(int32_t width, int32_t height) const {
    return width == static_cast<int32_t>(m_initial_width) &&
           height == static_cast<int32_t>(m_initial_height);
  }
#endif
};
