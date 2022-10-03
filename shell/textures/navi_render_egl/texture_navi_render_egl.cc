
#include "texture_navi_render_egl.h"

#include <dlfcn.h>
#include <chrono>

#include <flutter/fml/logging.h>
#include <flutter/fml/paths.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cmath>
#include <memory>

#include "backend/gl_process_resolver.h"
#include "backend/wayland_egl.h"
#include "view/flutter_view.h"

constexpr uint32_t kMapProviderTextureId = 98765;

constexpr uint32_t kIndexRender = 0;
constexpr uint32_t kIndexRoute = 1;

constexpr char kNaviRenderSoName[] = "libnav_render.so";
constexpr char kNaviRoutingSoName[] = "libnav_routing.so";

constexpr char kMapCacheDir[] = ".navigation";

TextureNaviRender::TextureNaviRender(FlutterView* view)
    : Texture(kMapProviderTextureId, GL_TEXTURE_2D, GL_RGBA8, Create, Dispose),
      m_egl_backend(reinterpret_cast<WaylandEglBackend*>(view->GetBackend())) {
  m_h_module[kIndexRender] = dlopen(kNaviRenderSoName, RTLD_NOW | RTLD_GLOBAL);
  if (m_h_module[kIndexRender]) {
    InitRenderApi();

    FML_LOG(INFO) << "navigation render interface version: "
                  << m_render_api.version();

    m_map_base_path.assign(GetCachePath());

    FML_LOG(INFO) << "Navi Base Path: " << m_map_base_path;
  }

#if defined(BUILD_TEXTURE_NAVI_EGL_ROUTING)
  m_h_module[kIndexRoute] = dlopen(kNaviRoutingSoName, RTLD_NOW | RTLD_GLOBAL);
  if (m_h_module[kIndexRoute]) {
    InitRoutingApi();

    FML_LOG(INFO) << "navigation routing interface version: "
                  << m_routing_api.version();
  }
#endif
}

TextureNaviRender::~TextureNaviRender() {
  if (m_h_module[kIndexRender]) {
    m_render_api.de_initialize(m_render_api.ctx);
    dlclose(m_h_module[0]);
  }
#if defined(BUILD_TEXTURE_NAVI_EGL_ROUTING)
  if (m_h_module[kIndexRoute]) {
    m_routing_api.de_initialize(m_routing_api.ctx);
    dlclose(m_h_module[1]);
  }
#endif
}

flutter::EncodableValue TextureNaviRender::Create(void* userdata) {
  auto* obj = reinterpret_cast<TextureNaviRender*>(userdata);

  obj->m_egl_backend->MakeTextureCurrent();

  if (!obj->m_initialized && obj->m_render_api.version
#if defined(BUILD_TEXTURE_NAVI_EGL_ROUTING)
      && obj->m_routing_api.version
#endif
  ) {
    FML_DLOG(INFO) << "Initializing Navigation Texture (" << obj->m_width
                   << " x " << obj->m_height << ")";

    /// GL Resolver
    obj->m_render_api.gl_loader(
        userdata, [](void* userdata, char const* process_name) -> const void* {
          return GlProcessResolver::GetInstance().process_resolver(
              process_name);
        });

    obj->m_render_api.ctx =
        obj->m_render_api.initialize("", obj->m_width, obj->m_height, "");
    if (nullptr == obj->m_render_api.ctx) {
      return flutter::EncodableValue(flutter::EncodableMap{
          {flutter::EncodableValue("result"), flutter::EncodableValue(-1)},
          {flutter::EncodableValue("error"),
           flutter::EncodableValue("Navigation Failed to Initialize")}});
    }

    assert(obj->m_render_api.ctx);

#if defined(BUILD_TEXTURE_NAVI_EGL_ROUTING)
    obj->m_routing_api.ctx = obj->m_routing_api.initialize("", "");
    assert(obj->m_render_api.ctx);
#endif

    glViewport(0, 0, obj->m_width, obj->m_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

    /// Framebuffer
    glGenFramebuffers(1, &obj->m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, obj->m_fbo);

    /// Output Texture
    glGenTextures(1, &obj->m_rendered_texture);
    glBindTexture(GL_TEXTURE_2D, obj->m_rendered_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, obj->m_width, obj->m_height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           obj->m_rendered_texture, 0);

    GLenum DrawBuffers[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, DrawBuffers);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      return flutter::EncodableValue(flutter::EncodableMap{
          {flutter::EncodableValue("result"), flutter::EncodableValue(-1)},
          {flutter::EncodableValue("error"),
           flutter::EncodableValue("Bad framebuffer status")}});
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glFinish();
    obj->m_egl_backend->ClearCurrent();

    obj->Enable(obj->m_rendered_texture);

    obj->m_initialized = true;

    return flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue("result"), flutter::EncodableValue(0)},
        {flutter::EncodableValue("textureId"),
         flutter::EncodableValue(obj->m_name)},
        {flutter::EncodableValue("width"),
         flutter::EncodableValue(obj->m_width)},
        {flutter::EncodableValue("height"),
         flutter::EncodableValue(obj->m_height)},
        {flutter::EncodableValue("render_ctx"),
         flutter::EncodableValue(
             reinterpret_cast<int64_t>(obj->m_render_api.ctx))}
#if defined(BUILD_TEXTURE_NAVI_EGL_ROUTING)
        ,
        {flutter::EncodableValue("routing_ctx"),
         flutter::EncodableValue(
             reinterpret_cast<int64_t>(obj->m_routing_api.ctx))}
#endif
    });
  }
  return flutter::EncodableValue(flutter::EncodableMap{
      {flutter::EncodableValue("result"), flutter::EncodableValue(-1)},
      {flutter::EncodableValue("error"),
       flutter::EncodableValue("Modules not loaded")}});
}

void TextureNaviRender::Dispose(void* userdata) {
  auto* obj = (TextureNaviRender*)userdata;
  obj->Disable();
}

void TextureNaviRender::Draw(void* userdata) {
  auto* obj = (TextureNaviRender*)userdata;

  if (!m_draw_next || !m_initialized)
    return;

  obj->m_egl_backend->MakeTextureCurrent();

  glClear(GL_COLOR_BUFFER_BIT);
  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
  glViewport(0, 0, m_width, m_height);

  double now = std::chrono::duration_cast<std::chrono::duration<double>>(
                   std::chrono::high_resolution_clock::now().time_since_epoch())
                   .count();

  obj->m_render_api.render(obj->m_render_api.ctx, now);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  obj->m_egl_backend->ClearCurrent();

  m_draw_next = false;
  obj->FrameReady();
}

std::string TextureNaviRender::GetCachePath() {
  const char* homedir;
  if ((homedir = getenv("HOME")) == nullptr) {
    homedir = getpwuid(getuid())->pw_dir;
  }

  auto path = fml::paths::JoinPaths({homedir, kMapCacheDir});

  mkdir(path.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

  return path;
}

void TextureNaviRender::InitRenderApi() {
  m_render_api.version = reinterpret_cast<NAV_RENDER_API_VERSION_T*>(
      dlsym(m_h_module[kIndexRender], "nav_render_version"));
  assert(m_render_api.version);

  /// fail if version is not as expected

  m_render_api.gl_loader = reinterpret_cast<NAV_RENDER_API_LOAD_GL_FUNCTIONS*>(
      dlsym(m_h_module[kIndexRender], "nav_render_load_gl_functions"));
  assert(m_render_api.gl_loader);
  m_render_api.initialize = reinterpret_cast<NAV_RENDER_API_INITIALIZE_T*>(
      dlsym(m_h_module[kIndexRender], "nav_render_initialize"));
  assert(m_render_api.initialize);
  m_render_api.de_initialize =
      reinterpret_cast<NAV_RENDER_API_DE_INITIALIZE_T*>(
          dlsym(m_h_module[kIndexRender], "nav_render_de_initialize"));
  assert(m_render_api.de_initialize);
  m_render_api.render = reinterpret_cast<NAV_RENDER_API_RENDER_T*>(
      dlsym(m_h_module[kIndexRender], "nav_render_render"));
  assert(m_render_api.render);
  m_render_api.resize = reinterpret_cast<NAV_RENDER_API_RESIZE_T*>(
      dlsym(m_h_module[kIndexRender], "nav_render_resize"));
  assert(m_render_api.resize);
}

#if defined(BUILD_TEXTURE_NAVI_EGL_ROUTING)
void TextureNaviRender::InitRoutingApi() {
  m_routing_api.version = reinterpret_cast<NAV_ROUTING_API_VERSION_T*>(
      dlsym(m_h_module[kIndexRoute], "nav_routing_version"));
  assert(m_routing_api.version);

  /// fail if version is not as expected

  m_routing_api.initialize = reinterpret_cast<NAV_ROUTING_API_INITIALIZE_T*>(
      dlsym(m_h_module[kIndexRoute], "nav_routing_initialize"));
  assert(m_routing_api.initialize);

  m_routing_api.de_initialize =
      reinterpret_cast<NAV_ROUTING_API_DE_INITIALIZE_T*>(
          dlsym(m_h_module[kIndexRoute], "nav_routing_de_initialize"));
  assert(m_routing_api.de_initialize);
}
#endif
