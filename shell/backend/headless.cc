#include "headless.h"
#include "logging.h"
#include "engine.h"

HeadlessBackend::HeadlessBackend(uint32_t initial_width,
                                 uint32_t initial_height)
    : Backend(this, Resize, CreateSurface),
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
  if (engine) {
    auto result = engine->SetWindowSize(static_cast<size_t>(b->m_height),
                                        static_cast<size_t>(b->m_width));
    if (result != kSuccess) {
      spdlog::error("Failed to set Flutter Engine Window Size");
    }
  }
}

void HeadlessBackend::CreateSurface(void* /* user_data */,
                                    size_t /* index */,
                                    wl_surface* /* surface */,
                                    int32_t /* width */,
                                    int32_t /* height */) {}

FlutterRendererConfig HeadlessBackend::GetRenderConfig() {
  return Backend::GetRenderConfig();
}

FlutterCompositor HeadlessBackend::GetCompositorConfig() {
  return Backend::GetCompositorConfig();
}
