#include "osmesa_egl_headless.h"
#include "logging.h"
#include "engine.h"
#include "egl_headless.h"
#include "osmesa_process_resolver.h"
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"

OSMesaBackend::OSMesaBackend(uint32_t initial_width,
                              uint32_t initial_height,
                              const bool debug_backend,
                              const int buffer_size)
    : Egl_headless(buffer_size, debug_backend),
      Backend(this, Resize, CreateSurface),
      m_prev_width(initial_width),
      m_prev_height(initial_height) {}

void OSMesaBackend::Resize(void* user_data,
                             size_t /* index */,
                             Engine* engine,
                             int32_t width,
                             int32_t height) {
  auto b = reinterpret_cast<OSMesaBackend*>(user_data);
  b->m_prev_width = b->m_width;
  b->m_prev_height = b->m_height;
  b->m_width = static_cast<uint32_t>(width);
  b->m_height = static_cast<uint32_t>(height);
  if (engine) {
    auto result = engine->SetWindowSize(static_cast<size_t>(b->m_height),
                                        static_cast<size_t>(b->m_width));
    if (result != kSuccess) {
      spdlog::error("Failed to set Flutter Engine Window Size");
    }
  }
}

void OSMesaBackend::CreateSurface(void* /* user_data */,
                                    size_t /* index */,
                                    wl_surface* /* surface */,
                                    int32_t /* width */,
                                    int32_t /* height */) {}

FlutterRendererConfig OSMesaBackend::GetRenderConfig() {
  return Backend::GetRenderConfig();
}

FlutterCompositor OSMesaBackend::GetCompositorConfig() {
  return Backend::GetCompositorConfig();
}
