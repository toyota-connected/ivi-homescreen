// Copyright 2020-2022 Toyota Connected North America
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

#pragma once

#include <string>
#include <vector>
#include "rapidjson/document.h"

class Configuration {
 public:
  struct Config {
    std::string app_id;
    std::string json_configuration_path;
    std::string cursor_theme;
    bool disable_cursor;
    bool debug_backend;

    struct {
      std::vector<std::string> command_line_args;
      std::string bundle_path;
      std::string window_type;
      int32_t accessibility_features;
      uint32_t width;
      uint32_t height;
      bool fullscreen;
    } view;
  };

  static std::vector<struct Configuration::Config> ParseConfig(
      struct Configuration::Config& config);

  static void PrintConfig(const Config&);

 private:
  static constexpr char kViewKey[] = "view";
  static constexpr char kBundlePathKey[] = "bundle_path";
  static constexpr char kWindowTypeKey[] = "window_type";
  static constexpr char kWidthKey[] = "width";
  static constexpr char kHeightKey[] = "height";
  static constexpr char kAccessibilityFeaturesKey[] = "accessibility_features";
  static constexpr char kAppIdKey[] = "app_id";
  static constexpr char kCursorThemeKey[] = "cursor_theme";
  static constexpr char kDisableCursorKey[] = "disable_cursor";
  static constexpr char kDebugBackendKey[] = "debug_backend";
  static constexpr char kCommandLineArgsKey[] = "command_line_args";
  static constexpr char kFullscreenKey[] = "fullscreen";

  static void Validate(rapidjson::Document& doc);

  static rapidjson::Document getJsonDocument(const std::string& filename);

  static rapidjson::SizeType getViewCount(rapidjson::Document& doc);

  static void getViewParameters(
      const rapidjson::GenericValue<rapidjson::UTF8<>>::Object& obj,
      Config& instance);

  static void getGlobalParameters(
      const rapidjson::GenericValue<rapidjson::UTF8<>>::Object& obj,
      Config& instance);

  static void getView(rapidjson::Document& doc, int index, Config& instance);

  static void getCliOverrides(rapidjson::Document& doc,
                              Config& instance,
                              Config& cli);
};
