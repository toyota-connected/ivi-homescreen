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

#ifndef PLUGINS_COMMON_GLIB_MAIN_LOOP_H_
#define PLUGINS_COMMON_GLIB_MAIN_LOOP_H_

#include <memory>
#include <thread>

extern "C" {
#include <glib.h>
}

namespace plugin_common::glib {

class MainLoop {
 public:
  virtual ~MainLoop();

  // Returns the shared MainLoop instance.
  static const MainLoop& GetInstance();

  void ExitLoop() { exit_loop_ = true; }

  // Prevent copying.
  MainLoop(MainLoop const&) = delete;
  MainLoop& operator=(MainLoop const&) = delete;

 protected:
  // Clients should always use GetInstance().
  MainLoop();

 private:
  std::unique_ptr<std::thread> gthread_;
  GMainLoop* main_loop_{};
  GMainContext* context_{};

  bool exit_loop_{};
  volatile bool is_running_{};

  static void main_loop(MainLoop* data);
};

}  // namespace plugin_common::glib

#endif  // PLUGINS_COMMON_GLIB_MAIN_LOOP_H_