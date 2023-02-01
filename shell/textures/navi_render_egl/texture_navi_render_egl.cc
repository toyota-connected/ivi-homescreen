
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

TextureNaviRender::TextureNaviRender(FlutterView* view)
    : Texture(kMapProviderTextureId, GL_TEXTURE_2D, GL_RGBA8, Create, Dispose),
      m_egl_backend(reinterpret_cast<WaylandEglBackend*>(view->GetBackend())) {}

TextureNaviRender::~TextureNaviRender() {
  if (m_h_module) {
    for (auto ctx : m_render_api.ctx) {
      m_render_api.de_initialize(ctx);
    }
    dlclose(m_h_module);
  }
}

flutter::EncodableValue TextureNaviRender::Create(
    void* userdata,
    const std::map<flutter::EncodableValue, flutter::EncodableValue>* args) {
  auto* obj = reinterpret_cast<TextureNaviRender*>(userdata);

  std::string access_token;
  auto it = args->find(flutter::EncodableValue("access_token"));
  if (it != args->end()) {
    access_token = std::get<std::string>(it->second);
  }

  std::string module;
  it = args->find(flutter::EncodableValue("module"));
  if (it != args->end()) {
    module = std::get<std::string>(it->second);
  }
  if (module.empty()) {
    module = kNaviRenderSoName;
    FML_LOG(INFO) << "\"module\" not set, using " << module;
  }

  std::string asset_path;
  it = args->find(flutter::EncodableValue("asset_path"));
  if (it != args->end()) {
    asset_path = std::get<std::string>(it->second);
  }
  if (asset_path.empty()) {
    FML_LOG(ERROR) << "\"asset_path\" not set!!";
  }

  std::string cache_folder;
  it = args->find(flutter::EncodableValue("cache_folder"));
  if (it != args->end()) {
    cache_folder = std::get<std::string>(it->second);
  }
  if (cache_folder.empty()) {
    FML_LOG(ERROR) << "\"cache_folder\" not set!!";
  }
  mkdir(cache_folder.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

  std::string misc_folder;
  it = args->find(flutter::EncodableValue("misc_folder"));
  if (it != args->end()) {
    misc_folder = std::get<std::string>(it->second);
  }
  if (cache_folder.empty()) {
    FML_LOG(ERROR) << "\"misc_folder\" not set!!";
  }

  if (!obj->m_h_module) {
    obj->m_h_module = dlopen(kNaviRenderSoName, RTLD_NOW | RTLD_GLOBAL);
    if (obj->m_h_module) {
      obj->InitRenderApi();
      FML_LOG(INFO) << "navigation render interface version: "
                    << obj->m_render_api.version();
    }
  }

  if (!obj->m_render_api.version) {
    return flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue("result"), flutter::EncodableValue(-1)},
        {flutter::EncodableValue("error"),
         flutter::EncodableValue("Modules not loaded")}});
  }

  FML_DLOG(INFO) << "Initializing Navigation Texture (" << obj->m_width << " x "
                 << obj->m_height << ")";

  /// GL Resolver
  obj->m_render_api.gl_loader =
      reinterpret_cast<void (*)(void*, GlLoaderFunction)>(eglGetProcAddress);

  obj->m_egl_backend->MakeTextureCurrent();

  glViewport(0, 0, obj->m_width, obj->m_height);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

  /// Framebuffer
  glGenFramebuffers(1, &obj->m_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, obj->m_fbo);

  /// Output Texture
  GLuint textureId;
  glGenTextures(1, &textureId);
  glBindTexture(GL_TEXTURE_2D, textureId);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, obj->m_width, obj->m_height, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         textureId, 0);

  GLenum DrawBuffers[1] = {GL_COLOR_ATTACHMENT0};
  glDrawBuffers(1, DrawBuffers);

  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    return flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue("result"), flutter::EncodableValue(-1)},
        {flutter::EncodableValue("error"),
         flutter::EncodableValue("Bad framebuffer status")}});
  }

  obj->m_render_api.ctx.push_back(obj->m_render_api.initialize(
      access_token.c_str(), obj->m_width, obj->m_height, asset_path.c_str(),
      cache_folder.c_str(), misc_folder.c_str(), &textureId));
  if (!obj->m_render_api.ctx.back()) {
    return flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue("result"), flutter::EncodableValue(-1)},
        {flutter::EncodableValue("error"),
         flutter::EncodableValue("Navigation Failed to Initialize")}});
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  glFinish();
  obj->m_egl_backend->ClearCurrent();

  obj->Enable(textureId);

  obj->m_initialized = true;

  return flutter::EncodableValue(flutter::EncodableMap{
      {flutter::EncodableValue("result"), flutter::EncodableValue(0)},
      {flutter::EncodableValue("textureId"),
      flutter::EncodableValue(textureId)},
      {flutter::EncodableValue("width"), flutter::EncodableValue(obj->m_width)},
      {flutter::EncodableValue("height"),
       flutter::EncodableValue(obj->m_height)},
      {flutter::EncodableValue("render_ctx"),
       flutter::EncodableValue(
           reinterpret_cast<int64_t>(obj->m_render_api.ctx.back()))}});
}

void TextureNaviRender::Dispose(void* userdata, GLuint name) {
  auto* obj = (TextureNaviRender*)userdata;
  obj->Disable(name);
}

void TextureNaviRender::Draw(void* userdata) {
  auto* obj = (TextureNaviRender*)userdata;

  if (!m_draw_next)  // || !m_initialized)
    return;

  for (auto ctx : obj->m_render_api.ctx) {
    obj->m_egl_backend->MakeTextureCurrent();

    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, m_width, m_height);

    double now =
        std::chrono::duration_cast<std::chrono::duration<double>>(
            std::chrono::high_resolution_clock::now().time_since_epoch())
            .count();

    obj->m_render_api.render(ctx, now);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    obj->m_egl_backend->ClearCurrent();

    m_draw_next = false;
    obj->FrameReady();
  }
}

void TextureNaviRender::InitRenderApi() {
  m_render_api.version = reinterpret_cast<NAV_RENDER_API_VERSION_T*>(
      dlsym(m_h_module, "nav_render_version"));

  if (m_render_api.version &&
      m_render_api.version() != EXPECTED_RENDER_API_VERSION) {
    assert(false);
  }

  m_render_api.gl_loader = reinterpret_cast<NAV_RENDER_API_LOAD_GL_FUNCTIONS*>(
      dlsym(m_h_module, "nav_render_load_gl_functions"));
  assert(m_render_api.gl_loader);
  m_render_api.initialize = reinterpret_cast<NAV_RENDER_API_INITIALIZE_T*>(
      dlsym(m_h_module, "nav_render_initialize"));
  assert(m_render_api.initialize);
  m_render_api.de_initialize =
      reinterpret_cast<NAV_RENDER_API_DE_INITIALIZE_T*>(
          dlsym(m_h_module, "nav_render_de_initialize"));
  assert(m_render_api.de_initialize);
  m_render_api.render = reinterpret_cast<NAV_RENDER_API_RENDER_T*>(
      dlsym(m_h_module, "nav_render_render"));
  assert(m_render_api.render);
  m_render_api.resize = reinterpret_cast<NAV_RENDER_API_RESIZE_T*>(
      dlsym(m_h_module, "nav_render_resize"));
  assert(m_render_api.resize);
}
