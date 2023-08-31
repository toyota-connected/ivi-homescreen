
#include "texture_navi_render_egl.h"

#include <dlfcn.h>
#include <mutex>

#include <sys/stat.h>
#include <cmath>
#include <memory>

#include "backend/gl_process_resolver.h"
#include "backend/wayland_egl.h"
#include "logging.h"
#include "view/flutter_view.h"

constexpr uint32_t kMapProviderTextureId = 98765;

constexpr char kNaviRenderSoName[] = "libnav_render.so";

std::mutex g_gl_mutex;

TextureNaviRender::TextureNaviRender(FlutterView* view)
    : Texture(kMapProviderTextureId, GL_TEXTURE_2D, GL_RGBA8, Create, Dispose),
      m_h_module(nullptr),
      m_egl_backend(reinterpret_cast<WaylandEglBackend*>(view->GetBackend())) {}

TextureNaviRender::~TextureNaviRender() {
  if (m_h_module) {
    for (auto const& [key, context] : m_render_api.ctx) {
      m_render_api.de_initialize(context);
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
  if (it != args->end() && !it->second.IsNull()) {
    access_token = std::get<std::string>(it->second);
  }

  std::string module;
  it = args->find(flutter::EncodableValue("module"));
  if (it != args->end() && !it->second.IsNull()) {
    module = std::get<std::string>(it->second);
  }
  if (module.empty()) {
    module = kNaviRenderSoName;
    spdlog::info("\"module\" not set, using {}", module);
  }

  bool map_flutter_assets = false;
  it = args->find(flutter::EncodableValue("map_flutter_assets"));
  if (it != args->end() && !it->second.IsNull()) {
    map_flutter_assets = std::get<bool>(it->second);
  }

  std::string asset_path;
  if (map_flutter_assets) {
    asset_path = obj->m_flutter_engine->GetAssetDirectory();
  } else {
    it = args->find(flutter::EncodableValue("asset_path"));
    if (it != args->end() && !it->second.IsNull()) {
      asset_path = std::get<std::string>(it->second);
    }
  }
  if (asset_path.empty()) {
    spdlog::error("\"asset_path\" not set!!");
  }

  std::string cache_folder;
  it = args->find(flutter::EncodableValue("cache_folder"));
  if (it != args->end() && !it->second.IsNull()) {
    cache_folder = std::get<std::string>(it->second);
  }
  if (cache_folder.empty()) {
    spdlog::error("\"cache_folder\" not set!!");
  }
  mkdir(cache_folder.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

  std::string misc_folder;
  it = args->find(flutter::EncodableValue("misc_folder"));
  if (it != args->end() && !it->second.IsNull()) {
    misc_folder = std::get<std::string>(it->second);
  }
  if (cache_folder.empty()) {
    spdlog::error("\"misc_folder\" not set!!");
  }

  obj->m_interface_version = 0;
  it = args->find(flutter::EncodableValue("intf_ver"));
  if (it != args->end() && !it->second.IsNull()) {
    obj->m_interface_version = std::get<int>(it->second);
  }

  if (!obj->m_h_module) {
    SPDLOG_DEBUG("Attempting to open [{}]", module);
    obj->m_h_module = dlopen(module.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (obj->m_h_module) {
      obj->InitRenderApi();
      spdlog::info("navigation render interface version: 0x{:x}",
                   obj->m_render_api.version());
    } else {
      return flutter::EncodableValue(flutter::EncodableMap{
          {flutter::EncodableValue("result"), flutter::EncodableValue(-1)},
          {flutter::EncodableValue("error"),
           flutter::EncodableValue("Failed to load module")}});
    }
  }

  SPDLOG_DEBUG("Initializing Navigation Texture ({} x {})", obj->m_width,
               obj->m_height);

  std::lock_guard<std::mutex> guard(g_gl_mutex);

  obj->m_egl_backend->MakeTextureCurrent();

  glViewport(0, 0, obj->m_width, obj->m_height);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

  /// Framebuffer
  glGenFramebuffers(1, &obj->m_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, obj->m_fbo);

  /// Output Texture
  glGenTextures(1, &obj->m_texture_id);
  glBindTexture(GL_TEXTURE_2D, obj->m_texture_id);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, obj->m_width, obj->m_height, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         obj->m_texture_id, 0);

  glGenRenderbuffers(1, &obj->m_rbo);
  glBindRenderbuffer(GL_RENDERBUFFER, obj->m_rbo);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, obj->m_width,
                        obj->m_height);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                            GL_RENDERBUFFER, obj->m_rbo);

  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    return flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue("result"), flutter::EncodableValue(-1)},
        {flutter::EncodableValue("error"),
         flutter::EncodableValue("Bad framebuffer status")}});
  }

  NAV_RENDER_API_CONTEXT_T* context = nullptr;
  if (obj->m_interface_version < 2) {
    context = obj->m_render_api.initialize(
        access_token.c_str(), obj->m_width, obj->m_height, asset_path.c_str(),
        cache_folder.c_str(), misc_folder.c_str());
  } else if (obj->m_interface_version == 2) {
    NavRenderConfig nav_render_init = {
        .access_token = access_token.c_str(),
        .width = obj->m_width,
        .height = obj->m_height,
        .asset_path = asset_path.c_str(),
        .cache_folder = cache_folder.c_str(),
        .misc_folder = misc_folder.c_str(),
        .pfnLogger =
            [](int level, const char* /* context */, const char* message) {
              switch (level) {
                case SPDLOG_LEVEL_TRACE:
                  spdlog::trace(message);
                  break;
                case SPDLOG_LEVEL_DEBUG:
                  spdlog::debug(message);
                  break;
                case SPDLOG_LEVEL_INFO:
                  spdlog::info(message);
                  break;
                case SPDLOG_LEVEL_WARN:
                  spdlog::warn(message);
                  break;
                case SPDLOG_LEVEL_ERROR:
                  spdlog::error(message);
                  break;
                case SPDLOG_LEVEL_CRITICAL:
                  spdlog::critical(message);
                  break;
                default:
                case SPDLOG_LEVEL_OFF:
                  break;
              }
            },
        .pfnGlLoader = [](void* /* userdata */,
                          const char* procname) -> const void* {
          return GlProcessResolver::GetInstance().process_resolver(procname);
        }};
    nav_render_init.dpy = obj->m_egl_backend->GetDisplay();
    nav_render_init.context = obj->m_egl_backend->GetTextureContext();
    nav_render_init.framebufferId = obj->m_fbo;
    context = obj->m_render_api.initialize2(&nav_render_init);
  }

  if (!context) {
    return flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue("result"), flutter::EncodableValue(-1)},
        {flutter::EncodableValue("error"),
         flutter::EncodableValue("Navigation Failed to Initialize")}});
  }
  obj->m_render_api.ctx[obj->m_texture_id] = context;

  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  glFinish();
  obj->m_egl_backend->ClearCurrent();

  obj->Enable(obj->m_texture_id);

  obj->m_run_enable = true;

  return flutter::EncodableValue(flutter::EncodableMap{
      {flutter::EncodableValue("result"), flutter::EncodableValue(0)},
      {flutter::EncodableValue("textureId"),
       flutter::EncodableValue(static_cast<int64_t>(obj->m_texture_id))},
      {flutter::EncodableValue("width"), flutter::EncodableValue(obj->m_width)},
      {flutter::EncodableValue("height"),
       flutter::EncodableValue(obj->m_height)},
      {flutter::EncodableValue("render_ctx"),
       flutter::EncodableValue(reinterpret_cast<int64_t>(context))}});
}

void TextureNaviRender::Dispose(void* userdata, GLuint name) {
  auto* obj = (TextureNaviRender*)userdata;
  auto context = obj->m_render_api.ctx[obj->m_texture_id];
  obj->m_run_enable = false;
  obj->m_render_api.de_initialize(context);
  obj->Disable(name);
  std::lock_guard<std::mutex> guard(g_gl_mutex);
  glDeleteTextures(1, &obj->m_texture_id);
  glDeleteRenderbuffers(1, &obj->m_rbo);
  glDeleteFramebuffers(1, &obj->m_fbo);
}

void TextureNaviRender::RunTask(void* userdata) {
  auto* obj = (TextureNaviRender*)userdata;
  for (auto [key, context] : obj->m_render_api.ctx) {
    if (obj->m_run_enable) {
      obj->m_render_api.run_task(context);
    }
  }
}

void TextureNaviRender::Draw(void* userdata) {
  auto* obj = (TextureNaviRender*)userdata;

  if (!obj->m_draw_next || !obj->m_run_enable)
    return;

  for (auto const& [key, context] : obj->m_render_api.ctx) {
    std::lock_guard<std::mutex> guard(g_gl_mutex);
    if (obj->m_interface_version == 2) {
      obj->m_render_api.render2(context);
    } else {
      obj->m_egl_backend->MakeTextureCurrent();

      glClear(GL_COLOR_BUFFER_BIT);
      glBindFramebuffer(GL_FRAMEBUFFER, obj->m_fbo);
      glViewport(0, 0, obj->m_width, obj->m_height);

      obj->m_render_api.render(context, obj->m_fbo);

      glBindFramebuffer(GL_FRAMEBUFFER, 0);

      obj->m_egl_backend->ClearCurrent();
    }
    obj->m_draw_next = false;
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

  m_render_api.initialize = reinterpret_cast<NAV_RENDER_API_INITIALIZE_T*>(
      dlsym(m_h_module, "nav_render_initialize"));
  m_render_api.initialize2 = reinterpret_cast<NAV_RENDER_API_INITIALIZE2_T*>(
      dlsym(m_h_module, "nav_render_initialize2"));

  assert(m_render_api.initialize || m_render_api.initialize2);

  m_render_api.de_initialize =
      reinterpret_cast<NAV_RENDER_API_DE_INITIALIZE_T*>(
          dlsym(m_h_module, "nav_render_de_initialize"));
  assert(m_render_api.de_initialize);
  m_render_api.run_task = reinterpret_cast<NAV_RENDER_API_RUN_TASK_T*>(
      dlsym(m_h_module, "nav_render_run_task"));
  assert(m_render_api.run_task);
  if (m_interface_version == 2) {
    m_render_api.render2 = reinterpret_cast<NAV_RENDER_API_RENDER2_T*>(
        dlsym(m_h_module, "nav_render_render2"));
    assert(m_render_api.render2);
  } else {
    m_render_api.render = reinterpret_cast<NAV_RENDER_API_RENDER_T*>(
        dlsym(m_h_module, "nav_render_render"));
    assert(m_render_api.render);
  }
  m_render_api.resize = reinterpret_cast<NAV_RENDER_API_RESIZE_T*>(
      dlsym(m_h_module, "nav_render_resize"));
  assert(m_render_api.resize);
}
