
#pragma once

#include <map>
#include <string>

#include <GLES3/gl32.h>
#include <flutter/encodable_value.h>

#include "render_api.h"

#if defined(BUILD_TEXTURE_NAVI_EGL_ROUTING)
#include "routing_api.h"
#endif

class App;

class Engine;

class FlutterView;

class WaylandEglBackend;

class TextureNaviRender { //TODO  : public Texture
 public:
  explicit TextureNaviRender(const FlutterView* view);

  ~TextureNaviRender();

  TextureNaviRender(const TextureNaviRender&) = delete;

  const TextureNaviRender& operator=(const TextureNaviRender&) = delete;

  /**
   * @brief Draw a new texture
   * @param[in,out] userdata Pointer to TextureNaviRender
   * @return void
   * @relation
   * wayland, flutter
   */
  static void Draw(void* userdata);

  /**
   * @brief Run Task
   * @param[in,out] userdata Pointer to TextureNaviRender
   * @return void
   * @relation
   * wayland, flutter
   */
  static void RunTask(void* userdata);

  /**
   * @brief Dispose Navigation Instance
   * @param[in,out] userdata Pointer to TextureNaviRender
   * @param[in] name Texture2d id
   * @return void
   * @relation
   * wayland, flutter
   */
  static void Dispose(void* userdata, GLuint name);

 private:
  static constexpr int EXPECTED_RENDER_API_VERSION = 0x00010002;

  WaylandEglBackend* m_egl_backend;
  std::string m_map_base_path;
  volatile bool m_run_enable{};

  GLuint m_fbo{};
  GLuint m_texture_id{};
  GLuint m_rbo{};
  int m_interface_version{};

  GLsizei m_width, m_height;

  static std::map<std::string, std::string> m_styles;

  /**
   * @brief Create Navigation Instance
   * @param[in,out] userdata Pointer to TextureNaviRender
   * @param[in] args from Dart
   * @return flutter::EncodableValue
   * @retval EncodableValue This contain result:OK, textureId, width, height and
   * render_ctx
   * @retval Error
   * @relation
   * wayland, flutter
   */
  static flutter::EncodableValue Create(
      void* userdata,
      const std::map<flutter::EncodableValue, flutter::EncodableValue>* args);

  struct {
    NAV_RENDER_API_VERSION_T* version{};

    std::map<GLuint, NAV_RENDER_API_CONTEXT_T*> ctx{};
    NAV_RENDER_API_INITIALIZE_T* initialize{};
    NAV_RENDER_API_INITIALIZE2_T* initialize2{};
    NAV_RENDER_API_DE_INITIALIZE_T* de_initialize{};
    NAV_RENDER_API_RUN_TASK_T* run_task{};
    NAV_RENDER_API_RENDER_T* render{};
    NAV_RENDER_API_RENDER2_T* render2{};
    NAV_RENDER_API_RESIZE_T* resize{};

  } m_render_api;

#if defined(BUILD_TEXTURE_NAVI_EGL_ROUTING)
  NAV_ROUTING_API_CONTEXT_T* m_routing_context{};

  struct {
    NAV_ROUTING_API_VERSION_T* version{};
    NAV_ROUTING_API_CONTEXT_T* ctx{};
    NAV_ROUTING_API_INITIALIZE_T* initialize{};
    NAV_ROUTING_API_DE_INITIALIZE_T* de_initialize{};

  } m_routing_api;
#endif

  void* m_h_module{};

  /**
   * @brief Initialize RenderApi
   * @return void
   * @relation
   * wayland, flutter
   */
  void InitRenderApi();

#if defined(BUILD_TEXTURE_NAVI_EGL_ROUTING)
  /**
   * @brief Initialize RoutingApi
   * @return void
   * @relation
   * wayland, flutter
   */
  void InitRoutingApi();
#endif
};
