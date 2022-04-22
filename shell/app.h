/*
 * Copyright 2020 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <EGL/egl.h>
#include <flutter_embedder.h>
#include <wayland-client.h>
#include <memory>

#include "constants.h"
#ifdef ENABLE_TEXTURE_TEST
#include "textures/test/texture_test.h"
#endif
#ifdef ENABLE_PLUGIN_TEXT_INPUT
#include "static_plugins/text_input/text_input.h"
#endif

class GlResolver;
class Display;
class EglWindow;
class Engine;

class App {
 private:
  std::shared_ptr<GlResolver> m_gl_resolver;
  std::shared_ptr<Display> m_display;
  std::shared_ptr<EglWindow> m_egl_window[kEngineInstanceCount];
  std::shared_ptr<Engine> m_engine[kEngineInstanceCount];
  uint8_t m_fps_output;
  uint32_t m_fps_period;
  uint32_t m_fps_counter;
  long long m_fps_pretime;
#ifdef ENABLE_TEXTURE_TEST
  std::unique_ptr<TextureTest> m_texture_test;
#endif
#ifdef ENABLE_PLUGIN_TEXT_INPUT
  std::shared_ptr<TextInput> m_text_input;
#endif

 public:
  explicit App(const std::string& app_id,
               const std::vector<std::string>& command_line_args,
               const std::string& application_override_path,
               bool fullscreen,
               bool enable_cursor,
               bool debug_egl,
               uint32_t width,
               uint32_t height,
               const std::string& cursor_theme_name);
  App(const App&) = delete;
  const App& operator=(const App&) = delete;

  GlResolver* GetGlResolver();

  std::shared_ptr<EglWindow> GetEglWindow(size_t index) {
    return m_egl_window[index];
  }

  int Loop();
};
