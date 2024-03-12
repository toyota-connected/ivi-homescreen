
#include "nav_render_surface.h"

#include <flutter/standard_message_codec.h>
#include <plugins/common/common.h>

#include <utility>

#include "libnav_render.h"

namespace nav_render_view_plugin {

static constexpr int kExpectedRenderApiVersion = 0x00010002;

// static
void NavRenderSurface::RegisterWithRegistrar(
    flutter::PluginRegistrar* registrar,
    int32_t id,
    std::string viewType,
    int32_t direction,
    double top,
    double left,
    double width,
    double height,
    const std::vector<uint8_t>& params,
    std::string assetDirectory,
    FlutterDesktopEngineRef engine,
    PlatformViewAddListener addListener,
    PlatformViewRemoveListener removeListener,
    void* platform_view_context) {
  auto plugin = std::make_unique<NavRenderSurface>(
      id, std::move(viewType), direction, top, left, width, height, params,
      std::move(assetDirectory), engine, addListener, removeListener,
      platform_view_context);
  registrar->AddPlugin(std::move(plugin));
}

NavRenderSurface::NavRenderSurface(int32_t id,
                                   std::string viewType,
                                   int32_t direction,
                                   double top,
                                   double left,
                                   double width,
                                   double height,
                                   const std::vector<uint8_t>& params,
                                   std::string assetDirectory,
                                   FlutterDesktopEngineState* state,
                                   PlatformViewAddListener addListener,
                                   PlatformViewRemoveListener removeListener,
                                   void* platform_view_context)
    : PlatformView(id,
                   std::move(viewType),
                   direction,
                   top,
                   left,
                   width,
                   height),
      id_(id),
      view_(state->view_controller->view),
      callback_(nullptr),
      platformViewsContext_(platform_view_context),
      removeListener_(removeListener),
      flutterAssetsPath_(std::move(assetDirectory)) {
  SPDLOG_TRACE("++NavRenderSurface::NavRenderSurface");
  auto& codec = flutter::StandardMessageCodec::GetInstance();
  const auto decoded = codec.DecodeMessage(params.data(), params.size());
  const auto& creationParams =
      std::get_if<flutter::EncodableMap>(decoded.get());

  std::string access_token;
  std::string module;
  bool map_flutter_assets{};
  std::string asset_path;
  std::string cache_folder;
  std::string misc_folder;
  int interface_version = 0;

  for (const auto& it : *creationParams) {
    auto key = std::get<std::string>(it.first);

    if (key == "access_token") {
      if (std::holds_alternative<std::string>(it.second)) {
        access_token = std::get<std::string>(it.second);
      }
    } else if (key == "map_flutter_assets") {
      if (std::holds_alternative<bool>(it.second)) {
        map_flutter_assets = std::get<bool>(it.second);
      }
    } else if (key == "asset_path") {
      if (std::holds_alternative<std::string>(it.second)) {
        asset_path = std::get<std::string>(it.second);
      }
    } else if (key == "cache_folder") {
      if (std::holds_alternative<std::string>(it.second)) {
        cache_folder = std::get<std::string>(it.second);
      }
    } else if (key == "misc_folder") {
      if (std::holds_alternative<std::string>(it.second)) {
        misc_folder = std::get<std::string>(it.second);
      }
    } else if (key == "intf_ver") {
      if (std::holds_alternative<int>(it.second)) {
        interface_version = std::get<int>(it.second);
      }
    }
  }

  if (map_flutter_assets) {
    asset_path = assetDirectory;
  }

  /* Setup Wayland subsurface */
  display_ = view_->GetDisplay()->GetDisplay();
  egl_display_ = eglGetDisplay(display_);
  assert(egl_display_);

  auto compositor = view_->GetDisplay()->GetCompositor();
  surface_ = wl_compositor_create_surface(compositor);
  egl_window_ = wl_egl_window_create(surface_, width_, height_);
  assert(egl_window_);

  InitializeEGL();
  egl_surface_ = eglCreateWindowSurface(
      egl_display_, egl_config_,
      reinterpret_cast<EGLNativeWindowType>(egl_window_), nullptr);

  // Subsurface
  auto sub_compositor = view_->GetDisplay()->GetSubCompositor();
  parent_surface_ = view_->GetWindow()->GetBaseSurface();
  subsurface_ = wl_subcompositor_get_subsurface(sub_compositor, surface_,
                                                parent_surface_);

  wl_subsurface_set_desync(subsurface_);

  eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_);

  if (!LibNavRender::IsPresent()) {
    spdlog::error("[NavRenderViewPlugin] libnav_render.so missing");
  }
  if (kExpectedRenderApiVersion != LibNavRender->GetInterfaceVersion()) {
    spdlog::error("[NavRenderViewPlugin] unexpected interface version: {}",
                  LibNavRender->GetInterfaceVersion());
  }

  if (interface_version == 2) {
    NavRenderConfig config{};
    // TODO
    auto context = LibNavRender->Initialize2(&config);
  }

  addListener(platformViewsContext_, id, &platform_view_listener_, this);

  SPDLOG_TRACE("--NavRenderSurface::NavRenderSurface");
}

NavRenderSurface::~NavRenderSurface() {
  SPDLOG_TRACE("++NavRenderSurface::~NavRenderSurface");
  if (callback_) {
    wl_callback_destroy(callback_);
    callback_ = nullptr;
  }

  if (subsurface_) {
    wl_subsurface_destroy(subsurface_);
    subsurface_ = nullptr;
  }

  if (egl_window_) {
    wl_egl_window_destroy(egl_window_);
    egl_window_ = nullptr;
  }

  if (surface_) {
    wl_surface_destroy(surface_);
    surface_ = nullptr;
  }
  removeListener_(platformViewsContext_, id_);
  SPDLOG_TRACE("--NavRenderSurface::~NavRenderSurface");
}

void NavRenderSurface::InitializeEGL() {
  SPDLOG_TRACE("++NavRenderSurface::InitializeEGL");
  EGLint major, minor;
  EGLBoolean ret = eglInitialize(egl_display_, &major, &minor);
  assert(ret == EGL_TRUE);

  ret = eglBindAPI(EGL_OPENGL_ES_API);
  assert(ret == EGL_TRUE);

  EGLint count;
  eglGetConfigs(egl_display_, nullptr, 0, &count);
  assert(count);
  SPDLOG_TRACE("[NavRenderView] EGL has {} configs", count);

  auto* configs = static_cast<EGLConfig*>(
      calloc(static_cast<size_t>(count), sizeof(EGLConfig)));
  assert(configs);

  EGLint n;
  ret = eglChooseConfig(egl_display_, kEglConfigAttribs.data(), configs, count,
                        &n);
  assert(ret && n >= 1);

  EGLint size;
  for (EGLint i = 0; i < n; i++) {
    eglGetConfigAttrib(egl_display_, configs[i], EGL_BUFFER_SIZE, &size);
    SPDLOG_TRACE("[NavRenderView] Buffer size for config {} is {}", i, size);
    if (buffer_size_ <= size) {
      memcpy(&egl_config_, &configs[i], sizeof(EGLConfig));
      break;
    }
  }
  free(configs);
  if (egl_config_ == nullptr) {
    SPDLOG_CRITICAL("[NavRenderView] did not find config with buffer size {}",
                    buffer_size_);
    assert(false);
  }

  egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT,
                                  kEglContextAttribs.data());
  assert(egl_context_);
  SPDLOG_TRACE("[NavRenderView] Context={}", egl_context_);
  SPDLOG_TRACE("--NavRenderSurface::InitializeEGL");
}

void NavRenderSurface::on_frame(void* data,
                                wl_callback* callback,
                                const uint32_t time) {
  const auto obj = static_cast<NavRenderSurface*>(data);

  obj->callback_ = nullptr;

  if (callback) {
    wl_callback_destroy(callback);
  }

  obj->DrawFrame(time);

  // Z-Order
  // wl_subsurface_place_above(obj->subsurface_, obj->parent_surface_);
  wl_subsurface_place_below(obj->subsurface_, obj->parent_surface_);

  obj->callback_ = wl_surface_frame(obj->surface_);
  wl_callback_add_listener(obj->callback_, &NavRenderSurface::frame_listener,
                           data);

  wl_subsurface_set_position(obj->subsurface_, obj->left_, obj->top_);

  wl_surface_commit(obj->surface_);
}

const wl_callback_listener NavRenderSurface::frame_listener = {.done =
                                                                   on_frame};

void NavRenderSurface::Resize(int32_t width, int32_t height) {
  SPDLOG_TRACE("[NavRenderView] Resize: {} {}", width, height);
  width_ = width;
  height_ = height;
}

void NavRenderSurface::DrawFrame(uint32_t /* time */) const {
  if (eglGetCurrentContext() != egl_context_) {
    eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_);
  }

  glViewport(0, 0, width_, height_);

  eglSwapBuffers(egl_display_, egl_surface_);
  eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void NavRenderSurface::SetOffset(int32_t left, int32_t top) {
  SPDLOG_DEBUG("[NavRenderSurface] SetOffset: left: {}, top: {}", left, top);
  wl_subsurface_set_position(subsurface_, left, top);
  if (!callback_) {
    on_frame(this, callback_, 0);
  }
}

void NavRenderSurface::on_resize(double width, double height, void* data) {
  SPDLOG_TRACE("[NavRenderSurface] on_resize: {} {}", width, height);
  auto plugin = static_cast<NavRenderSurface*>(data);
  plugin->Resize(static_cast<int32_t>(width), static_cast<int32_t>(height));
}

void NavRenderSurface::on_set_direction(int32_t direction, void* data) {
  SPDLOG_TRACE("[NavRenderSurface] on_set_direction: {}", direction);
  if (!data) {
    return;
  }
  static_cast<NavRenderSurface*>(data)->direction_ = direction;
}

void NavRenderSurface::on_set_offset(double left, double top, void* data) {
  SPDLOG_TRACE("[NavRenderSurface] on_set_offset: left: {}, top: {}", left,
               top);
  if (!data) {
    return;
  }
  static_cast<NavRenderSurface*>(data)->SetOffset(static_cast<int32_t>(left),
                                                  static_cast<int32_t>(top));
}

void NavRenderSurface::on_touch(int32_t action,
                                int32_t point_count,
                                const size_t point_data_size,
                                const double* /* point_data */,
                                void* data) {
  SPDLOG_TRACE(
      "[NavRenderSurface] on_touch: action: {}, point_count: {}, "
      "point_data_size: {}",
      action, point_count, point_data_size);
  if (!data) {
    return;
  }
  // auto plugin = static_cast<NavRenderSurface*>(data);
}

void NavRenderSurface::on_dispose(bool /* hybrid */, void* data) {
  // LibNavRender->DeInitialize(nav_render_context_);
}

const struct platform_view_listener NavRenderSurface::platform_view_listener_ =
    {.resize = on_resize,
     .set_direction = on_set_direction,
     .set_offset = on_set_offset,
     .on_touch = on_touch,
     .dispose = on_dispose};

}  // namespace nav_render_view_plugin
