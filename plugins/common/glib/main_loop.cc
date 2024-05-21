/*
 * Copyright 2023-2024 Toyota Connected North America
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

#include "main_loop.h"


namespace plugin_common_glib {

MainLoop::MainLoop()
    : gthread_(std::make_unique<std::thread>(main_loop, this)) {}

MainLoop::~MainLoop() = default;

const MainLoop& MainLoop::GetInstance() {
  static MainLoop sInstance;
  return sInstance;
};

void MainLoop::main_loop(MainLoop* data) {
  data->context_ = g_main_context_default();

  data->is_running_ = true;
  while (data->is_running_) {
    if (data->exit_loop_) {
      data->is_running_ = false;
    }

    g_main_context_iteration(data->context_, TRUE);
  }

  g_main_loop_quit(data->main_loop_);
  g_main_loop_unref(data->main_loop_);
}

}  // namespace plugin_common_glib
