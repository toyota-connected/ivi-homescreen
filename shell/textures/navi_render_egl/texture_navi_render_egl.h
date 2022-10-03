
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <flutter_embedder.h>

#include "../texture.h"
#include "render_api.h"
#if defined(BUILD_TEXTURE_NAVI_EGL_ROUTING)
#include "routing_api.h"
#endif

class App;

class Engine;

class FlutterView;

class WaylandEglBackend;

class TextureNaviRender : public Texture {
 public:
  explicit TextureNaviRender(FlutterView* view);

  ~TextureNaviRender();

  TextureNaviRender(const TextureNaviRender&) = delete;

  const TextureNaviRender& operator=(const TextureNaviRender&) = delete;

  void Draw(void* userdata);

 private:
  WaylandEglBackend* m_egl_backend;
  std::string m_map_base_path;
  bool m_initialized{};

  GLuint m_fbo{};
  GLuint m_rendered_texture{};

  static std::map<std::string, std::string> m_styles;

  static flutter::EncodableValue Create(void* userdata);

  static void Dispose(void* userdata);

  NAV_RENDER_API_CONTEXT_T* m_render_context{};

  struct {
    NAV_RENDER_API_VERSION_T* version{};

    NAV_RENDER_API_CONTEXT_T* ctx{};
    NAV_RENDER_API_LOAD_GL_FUNCTIONS* gl_loader{};
    NAV_RENDER_API_INITIALIZE_T* initialize{};
    NAV_RENDER_API_DE_INITIALIZE_T* de_initialize{};
    NAV_RENDER_API_RENDER_T* render{};
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

  void* m_h_module[2]{};

  void InitRenderApi();

#if defined(BUILD_TEXTURE_NAVI_EGL_ROUTING)
  void InitRoutingApi();
#endif

  static std::string GetCachePath();
};
