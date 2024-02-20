#include "headless.h"
#include "logging.h"
#include "engine.h"
#include "osmesa.h"
#include "osmesa_process_resolver.h"
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"

HeadlessBackend::HeadlessBackend(uint32_t initial_width,
                              uint32_t initial_height,
                              const bool debug_backend,
                              const int buffer_size)
    : OSMesaHeadless(),
      Backend(this, Resize, CreateSurface),
      m_prev_width(initial_width),
      m_prev_height(initial_height) {}

void HeadlessBackend::Resize(void* user_data,
                             size_t /* index */,
                             Engine* engine,
                             int32_t width,
                             int32_t height) {
  auto b = reinterpret_cast<HeadlessBackend*>(user_data);
  b->m_prev_width = b->m_width;
  b->m_prev_height = b->m_height;
  b->m_width = static_cast<uint32_t>(width);
  b->m_height = static_cast<uint32_t>(height);
  b->free_buffer();
  b->m_buf = b->create_osmesa_buffer(b->m_width, b->m_height);
  b->MakeCurrent();
  if (engine) {
    auto result = engine->SetWindowSize(static_cast<size_t>(b->m_height),
                                        static_cast<size_t>(b->m_width));
    if (result != kSuccess) {
      spdlog::error("Failed to set Flutter Engine Window Size");
    }
  }
}

void HeadlessBackend::CreateSurface(void* user_data,
                                      size_t /* index */,
                                      wl_surface* /*surface*/,
                                      int32_t width,
                                      int32_t height) {
  const auto b = static_cast<HeadlessBackend*>(user_data);
  b->m_width = width;
  b->m_height = height;
  b->m_buf = b->create_osmesa_buffer(b->m_width, b->m_height);
}

FlutterRendererConfig HeadlessBackend::GetRenderConfig() {
  return {.type = kOpenGL,
          .open_gl = {
              .struct_size = sizeof(FlutterOpenGLRendererConfig),
              .make_current = [](void* user_data) -> bool {
                const auto state =
                    static_cast<FlutterDesktopEngineState*>(user_data);
                return reinterpret_cast<HeadlessBackend*>(
                           state->view_controller->engine->GetBackend())
                    ->MakeCurrent();
              },
              .clear_current = [](void* userdata) -> bool {
                const auto state =
                    static_cast<FlutterDesktopEngineState*>(userdata);
                return reinterpret_cast<HeadlessBackend*>(
                           state->view_controller->engine->GetBackend())
                    ->ClearCurrent();
              },
              .fbo_callback = [](void*) -> uint32_t {
                return 0;  // FBO0
              },
              .fbo_reset_after_present = false,
              .gl_proc_resolver = [](void* /* userdata */,
                                     const char* name) -> void* {
                return GlProcessResolver::GetInstance().process_resolver(name);
              },
              .gl_external_texture_frame_callback =
                  [](void* userdata, const int64_t texture_id,
                     const size_t width, const size_t height,
                     FlutterOpenGLTexture* texture_out) -> bool {
                const auto state =
                    static_cast<FlutterDesktopEngineState*>(userdata);
                if (state->view_controller->engine->GetTextureObj(texture_id)) {
                  texture_out->name = static_cast<uint32_t>(texture_id);
                  texture_out->width = width;
                  texture_out->height = height;
#if defined(ENABLE_PLUGIN_OPENGL_TEXTURE)
                  state->view_controller->engine->GetTextureObj(texture_id)
                      ->GetFlutterOpenGLTexture(texture_out);
#endif
                  return true;
                }
                return false;
              },
              .present = [](void* userdata) -> bool {
                const auto state =
                    static_cast<FlutterDesktopEngineState*>(userdata);
                auto* b = reinterpret_cast<HeadlessBackend*>(
                    state->view_controller->engine->GetBackend());
                b->Finish();
                return true;
              },
          }};
}

FlutterCompositor HeadlessBackend::GetCompositorConfig() {
  return {.struct_size = sizeof(FlutterCompositor),
          .user_data = this,
          .create_backing_store_callback = nullptr,
          .collect_backing_store_callback = nullptr,
          .present_layers_callback = nullptr,
          .avoid_backing_store_cache = true};
}
