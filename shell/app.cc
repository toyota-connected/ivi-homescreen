// Copyright 2020 Toyota Connected North America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "app.h"

#include <sstream>
#include <thread>

#include <flutter/fml/logging.h>

#include "constants.h"
#include "display.h"
#include "egl_window.h"
#include "engine.h"
#include "gl_resolver.h"

App::App(const std::string& app_id,
         const std::vector<std::string>& command_line_args,
         const std::string& application_override_path,
         bool fullscreen,
         bool enable_cursor,
         bool debug_egl,
         bool sprawl,
         uint32_t width,
         uint32_t height,
         const std::string& cursor_theme_name)
    : m_gl_resolver(std::make_shared<GlResolver>()),
      m_display(
          std::make_shared<Display>(this, enable_cursor, cursor_theme_name)),
      m_egl_window{std::make_shared<EglWindow>(0,
                                               m_display,
                                               EglWindow::WINDOW_BG,
                                               app_id,
                                               fullscreen,
                                               debug_egl,
                                               sprawl,
                                               width,
                                               height)}
#ifdef ENABLE_TEXTURE_TEST
      ,
      m_texture_test(std::make_unique<TextureTest>(this))
#endif
#ifdef ENABLE_PLUGIN_TEXT_INPUT
      ,
      m_text_input(std::make_shared<TextInput>())
#endif
{

  FML_DLOG(INFO) << "+App::App";

  m_display->AglShellDoBackground(m_egl_window[0]->GetNativeSurface());

  std::vector<const char*> m_command_line_args_c;
  m_command_line_args_c.reserve(command_line_args.size());
  m_command_line_args_c.push_back(app_id.c_str());
  for (const auto& arg : command_line_args) {
    m_command_line_args_c.push_back(arg.c_str());
  }

  for (size_t i = 0; i < kEngineInstanceCount; i++) {
    m_engine[i] = std::make_shared<Engine>(this, i, m_command_line_args_c,
                                           application_override_path);
    m_engine[i]->Run(pthread_self());

    if (!m_engine[i]->IsRunning()) {
      FML_LOG(ERROR) << "Failed to Run Engine";
      exit(-1);
    }

    // Set Flutter Window Size
    auto result = m_engine[i]->SetWindowSize(m_egl_window[i]->GetHeight(),
                                             m_egl_window[i]->GetWidth());
    if (result != kSuccess) {
      FML_LOG(ERROR) << "Failed to set Flutter Engine Window Size";
    }

    FML_DLOG(INFO) << "(" << i << ") Engine running...";
  }

  // Enable pointer events
  m_display->SetEngine(m_engine[0]);

#ifdef ENABLE_TEXTURE_TEST
  m_texture_test->SetEngine(m_engine[0]);
#endif
#ifdef ENABLE_PLUGIN_TEXT_INPUT
  m_text_input->SetEngine(m_engine[0]);
  m_display->SetTextInput(m_text_input);
#endif

  m_display->AglShellDoReady();

  // init the fps output option.
  m_fps_output = 0;
  m_fps_period = 1;
  m_fps_counter = 0;

  const char* envstr_console;
  if ((envstr_console = getenv("FPS_OUTPUT_CONSOLE")) != nullptr) {
    int val = atoi(envstr_console);

    if (0 < val) {
      m_fps_output |= 0x01;
    }
  }

  const char* envstr_overlay;
  if ((envstr_overlay = getenv("FPS_OUTPUT_OVERLAY")) != nullptr) {
    int val = atoi(envstr_overlay);

    if (0 < val) {
      m_fps_output |= 0x02;
    }
  }

  const char* envstr_freq;
  if ((envstr_freq = getenv("FPS_OUTPUT_FREQUENCY")) != nullptr) {
    int val = atoi(envstr_freq);

    if (0 < val) {
      m_fps_period = val;
    }
  }

  if (0 < m_fps_output) {
    if (0 >= m_fps_period) {
      m_fps_period = 1;
    }

    m_fps_period *= (1000 / 16);
    m_fps_pretime = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
  }

  FML_DLOG(INFO) << "-App::App";
}

int App::Loop() {
  auto start_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();

  for (auto& i : m_engine) {
    i->RunTask();
  }

#ifdef ENABLE_TEXTURE_TEST
  if (m_texture_test) {
    m_texture_test->Draw(m_texture_test.get());
  }
#endif

  while (wl_display_prepare_read(m_display->GetDisplay()) != 0) {
    wl_display_dispatch_pending(m_display->GetDisplay());
  }
  wl_display_flush(m_display->GetDisplay());

  wl_display_read_events(m_display->GetDisplay());
  auto ret = wl_display_dispatch_pending(m_display->GetDisplay());

  auto end_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();

  auto elapsed = end_time - start_time;

  auto sleep_time = 16 - elapsed;

  if (sleep_time > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
  }

  // calc and output the fps.
  if (0 < m_fps_output) {
    m_fps_counter++;

    if (m_fps_period <= m_fps_counter) {
      auto fps_loop = (m_fps_counter * 1000) / (end_time - m_fps_pretime);
      auto fps_redraw = (m_egl_window[0]->GetFpsCounter() * 1000) /
                        (end_time - m_fps_pretime);

      m_fps_counter = 0;
      m_fps_pretime = end_time;

      if (0 < (m_fps_output & 0x01)) {
        if (0 < (m_fps_output & 0x01)) {
          FML_LOG(INFO) << "FPS = " << fps_loop << " " << fps_redraw;
        }

        if (0 < (m_fps_output & 0x02)) {
          m_egl_window[0]->DrawFps(fps_redraw);
        }
      }
    }
  }
  return ret;
}

GlResolver* App::GetGlResolver() {
  return m_gl_resolver.get();
}
