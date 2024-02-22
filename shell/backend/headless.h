
#pragma once

#include <cstdint>

#include "backend.h"
#include "constants.h"

#include "osmesa.h"

class Backend;

class Engine;

class HeadlessBackend : public OSMesaHeadless, public Backend {
 public:
  HeadlessBackend(uint32_t initial_width, uint32_t initial_height, const bool debug_backend, const int buffer_size);

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

  GLubyte* getHeadlessBuffer();


 private:
  uint32_t m_prev_width, m_width;
  uint32_t m_prev_height, m_height;
};
