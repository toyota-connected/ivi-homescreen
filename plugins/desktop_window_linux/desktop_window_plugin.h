/*
 * Copyright 2020-2023 Toyota Connected North America
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

#ifndef FLUTTER_PLUGIN_WINDOW_DESKTOP_LINUX_PLUGIN_H
#define FLUTTER_PLUGIN_WINDOW_DESKTOP_LINUX_PLUGIN_H

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>

#include "messages.h"

namespace desktop_window_linux_plugin {

class DesktopWindowLinuxPlugin final : public flutter::Plugin,
                                       public DesktopWindowLinuxApi {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar);

  DesktopWindowLinuxPlugin() = default;

  ~DesktopWindowLinuxPlugin() override = default;

  void getWindowSize(double& width, double& height) override;
  void setWindowSize(double width, double height) override;
  void setMinWindowSize(double width, double height) override;
  void setMaxWindowSize(double width, double height) override;
  void resetMaxWindowSize(double width, double height) override;
  void toggleFullScreen() override;
  void setFullScreen(bool set) override;
  bool getFullScreen() override;
  bool hasBorders() override;
  void setBorders(bool border) override;
  void toggleBorders() override;
  void focus() override;
  void stayOnTop(bool stayOnTop) override;

  // Disallow copy and assign.
  DesktopWindowLinuxPlugin(const DesktopWindowLinuxPlugin&) = delete;
  DesktopWindowLinuxPlugin& operator=(const DesktopWindowLinuxPlugin&) = delete;

 private:
  std::uint32_t m_width = 1024;
  std::uint32_t m_height = 768;
  std::uint32_t m_max_width = 0;
  std::uint32_t m_max_height = 0;
  std::uint32_t m_min_width = 0;
  std::uint32_t m_min_height = 0;
};
}  // namespace desktop_window_linux_plugin

#endif  // FLUTTER_PLUGIN_WINDOW_DESKTOP_LINUX_PLUGIN_H