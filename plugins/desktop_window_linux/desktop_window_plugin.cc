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

#include "desktop_window_plugin.h"

#include "messages.h"

#include "plugins/common/common.h"

namespace desktop_window_linux_plugin {

// static
void DesktopWindowLinuxPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrar* registrar) {
  auto plugin = std::make_unique<DesktopWindowLinuxPlugin>();

  DesktopWindowLinuxApi::SetUp(registrar->messenger(), plugin.get());

  registrar->AddPlugin(std::move(plugin));
}

void DesktopWindowLinuxPlugin::getWindowSize(double& width, double& height) {
  width = m_width;
  height = m_height;
  spdlog::debug("[desktop_window] getWindowSize: {} x {}", width, height);
}

void DesktopWindowLinuxPlugin::setWindowSize(double width, double height) {
  spdlog::debug("[desktop_window] setWindowSize: {} x {}", width, height);
  m_width = static_cast<uint32_t>(width);
  m_height = static_cast<uint32_t>(height);
}

void DesktopWindowLinuxPlugin::setMinWindowSize(double width, double height) {
  spdlog::debug("[desktop_window] setMinWindowSize: {} x {}", width, height);
  m_min_width = static_cast<std::uint32_t>(lround(width + 0.5));
  m_min_height = static_cast<std::uint32_t>(lround(height + 0.5));
}

void DesktopWindowLinuxPlugin::setMaxWindowSize(double width, double height) {
  spdlog::debug("[desktop_window] setMaxWindowSize: {} x {}", width, height);
  m_max_width = static_cast<std::uint32_t>(lround(width + 0.5));
  m_max_height = static_cast<std::uint32_t>(lround(height + 0.5));
}

void DesktopWindowLinuxPlugin::resetMaxWindowSize(double width, double height) {
  spdlog::debug("[desktop_window] resetMaxWindowSize: {} x {}", width, height);
  m_max_width = 0;
  m_max_height = 0;
}

void DesktopWindowLinuxPlugin::toggleFullScreen() {
  spdlog::debug("[desktop_window] toggleFullScreen");
}

void DesktopWindowLinuxPlugin::setFullScreen(bool set) {
  spdlog::debug("[desktop_window] setFullScreen: {}", set);
}

bool DesktopWindowLinuxPlugin::getFullScreen() {
  bool full_screen{};
  spdlog::debug("[desktop_window] getFullScreen: {}", full_screen);
  // TODO
  return full_screen;
}

bool DesktopWindowLinuxPlugin::hasBorders() {
  bool has_borders{};
  spdlog::debug("[desktop_window] hasBorders: {}", has_borders);
  // TODO
  return has_borders;
}

void DesktopWindowLinuxPlugin::setBorders(bool border) {
  spdlog::debug("[desktop_window] setBorders: {}", border);
  // TODO
}

void DesktopWindowLinuxPlugin::toggleBorders() {
  spdlog::debug("[desktop_window] toggleBorders");
  // TODO
}

void DesktopWindowLinuxPlugin::focus() {
  spdlog::debug("[desktop_window] focus");
  // TODO
}

void DesktopWindowLinuxPlugin::stayOnTop(bool stayOnTop) {
  spdlog::debug("[desktop_window] stayOnTop: {}", stayOnTop);
  // TODO
}

}  // namespace desktop_window_linux_plugin