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
#include "engine.h"
#include "wayland/display.h"
#include "wayland/window.h"
#if defined(BUILD_BACKEND_WAYLAND_EGL)
#include "backend/wayland_egl.h"
#elif defined(BUILD_BACKEND_WAYLAND_VULKAN)
#include "backend/wayland_vulkan.h"
#endif

App::App(const std::string& app_id,
         const std::vector<std::string>& command_line_args,
         const std::string& application_override_path,
         bool fullscreen,
         bool enable_cursor,
         bool debug_backend,
         uint32_t width,
         uint32_t height,
         const std::string& cursor_theme_name)
    : m_wayland_display(
          std::make_shared<Display>(enable_cursor, cursor_theme_name)),
#if defined(BUILD_BACKEND_WAYLAND_EGL)
      m_backend(std::make_shared<WaylandEglBackend>(
          m_wayland_display->GetDisplay(),
          m_wayland_display->GetBaseSurface(),
          debug_backend)),
#elif defined(BUILD_BACKEND_WAYLAND_VULKAN)
      m_backend(std::make_shared<WaylandVulkanBackend>(
          m_wayland_display->GetDisplay(),
          m_wayland_display->GetBaseSurface(),
          width,
          height,
          debug_backend)),
#endif
      m_wayland_window{
          std::make_shared<WaylandWindow>(0,
                                          m_wayland_display,
                                          m_wayland_display->GetBaseSurface(),
                                          WaylandWindow::WINDOW_BG,
                                          app_id,
                                          fullscreen,
                                          width,
                                          height,
                                          m_backend.get())}
#ifdef ENABLE_TEXTURE_TEST_EGL
      ,
      m_texture_test_egl(std::make_unique<TextureTestEgl>(this))
#endif
#ifdef ENABLE_PLUGIN_TEXT_INPUT
      ,
      m_text_input(std::make_shared<TextInput>())
#endif
{

  FML_DLOG(INFO) << "+App::App";

  std::vector<const char*> m_command_line_args_c;
  m_command_line_args_c.reserve(command_line_args.size());
  m_command_line_args_c.push_back(app_id.c_str());
  for (const auto& arg : command_line_args) {
    m_command_line_args_c.push_back(arg.c_str());
  }

  for (size_t i = 0; i < kEngineInstanceCount; i++) {
    m_flutter_engine[i] = std::make_shared<Engine>(
        this, i, m_command_line_args_c, application_override_path);
    m_flutter_engine[i]->Run(pthread_self());

    if (!m_flutter_engine[i]->IsRunning()) {
      FML_LOG(ERROR) << "Failed to Run Engine";
      exit(-1);
    }
    m_wayland_window[i]->SetEngine(m_flutter_engine[i]);

    FML_DLOG(INFO) << "(" << i << ") Engine running...";
  }

  // Enable pointer events
  m_wayland_display->SetEngine(m_flutter_engine[0]);

#ifdef ENABLE_TEXTURE_TEST_EGL
  m_texture_test_egl->SetEngine(m_flutter_engine[0]);
#endif
#ifdef ENABLE_PLUGIN_TEXT_INPUT
  m_text_input->SetEngine(m_flutter_engine[0]);
  m_wayland_display->SetTextInput(m_text_input);
#endif

  m_wayland_display->AglShellDoReady();

  // init the fps output option.
  m_fps.output = 0;
  m_fps.period = 1;
  m_fps.counter = 0;

  const char* env_string_console;
  if ((env_string_console = getenv("FPS_OUTPUT_CONSOLE")) != nullptr) {
    long val = strtol(env_string_console, nullptr, 10);

    if (0 < val) {
      m_fps.output |= 0x01;
    }
  }

  const char* env_string_overlay;
  if ((env_string_overlay = getenv("FPS_OUTPUT_OVERLAY")) != nullptr) {
    long val = strtol(env_string_overlay, nullptr, 10);

    if (0 < val) {
      m_fps.output |= 0x02;
    }
  }

  const char* env_string_freq;
  if ((env_string_freq = getenv("FPS_OUTPUT_FREQUENCY")) != nullptr) {
    long val = strtol(env_string_freq, nullptr, 10);

    if (0 < val) {
      m_fps.period = val;
    }
  }

  if (0 < m_fps.output) {
    if (0 >= m_fps.period) {
      m_fps.period = 1;
    }

    m_fps.period *= (1000 / 16);
    m_fps.pretime = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
  }

  FML_DLOG(INFO) << "-App::App";
}

int App::Loop() {
  auto start_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();

  for (auto& i : m_flutter_engine) {
    i->RunTask();
  }

#ifdef ENABLE_TEXTURE_TEST_EGL
  if (m_texture_test_egl) {
    m_texture_test_egl->Draw(m_texture_test_egl.get());
  }
#endif

  auto display = m_wayland_display->GetDisplay();
  while (wl_display_prepare_read(display) != 0) {
    wl_display_dispatch_pending(display);
  }
  wl_display_flush(display);

  wl_display_read_events(display);
  auto ret = wl_display_dispatch_pending(display);

  auto end_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();

  auto elapsed = end_time - start_time;

  auto sleep_time = 16 - elapsed;

  if (sleep_time > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
  }

  // calc and output the fps.
  if (0 < m_fps.output) {
    m_fps.counter++;

    if (m_fps.period <= m_fps.counter) {
      auto fps_loop = (m_fps.counter * 1000) / (end_time - m_fps.pretime);
      auto fps_redraw = (m_wayland_window[0]->GetFpsCounter() * 1000) /
                        (end_time - m_fps.pretime);

      m_fps.counter = 0;
      m_fps.pretime = end_time;

      if (0 < (m_fps.output & 0x01)) {
        if (0 < (m_fps.output & 0x01)) {
          FML_LOG(INFO) << "FPS = " << fps_loop << " " << fps_redraw;
        }

        if (0 < (m_fps.output & 0x02)) {
          m_wayland_window[0]->DrawFps(fps_redraw);
        }
      }
    }
  }
  return ret;
}
