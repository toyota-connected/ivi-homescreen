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

#include "configuration.h"

#include <fstream>
#include <sstream>

#include "constants.h"
#include "flutter/fml/logging.h"
#include "rapidjson/document.h"

rapidjson::SizeType Configuration::getViewCount(rapidjson::Document& doc) {
  if (!doc.HasMember(kViewKey)) {
    FML_LOG(ERROR) << "JSON Configuration must have a \"view\" object defined";
    exit(EXIT_FAILURE);
  }

  if (doc[kViewKey].IsArray()) {
    return doc[kViewKey].GetArray().Capacity();
  } else {
    return 1;
  }
}

void Configuration::getViewParameters(
    const rapidjson::GenericValue<rapidjson::UTF8<>>::Object& obj,
    Config& instance) {
  if (obj.HasMember(kBundlePathKey) && obj[kBundlePathKey].IsString()) {
    instance.view.bundle_path.assign(obj[kBundlePathKey].GetString());
  }
  if (obj.HasMember(kWindowTypeKey) && obj[kWindowTypeKey].IsString()) {
    instance.view.window_type.assign(obj[kWindowTypeKey].GetString());
  }
  if (obj.HasMember(kWidthKey) && obj[kWidthKey].IsInt()) {
    instance.view.width = obj[kWidthKey].GetInt();
  }
  if (obj.HasMember(kHeightKey) && obj[kHeightKey].IsInt()) {
    instance.view.height = obj[kHeightKey].GetInt();
  }
  if (obj.HasMember(kAccessibilityFeaturesKey) &&
      obj[kAccessibilityFeaturesKey].IsInt()) {
    instance.view.accessibility_features =
        obj[kAccessibilityFeaturesKey].GetInt();
  }
}

void Configuration::getGlobalParameters(
    const rapidjson::GenericValue<rapidjson::UTF8<>>::Object& obj,
    Config& instance) {
  if (obj.HasMember(kAppIdKey) && obj[kAppIdKey].IsString()) {
    instance.app_id.assign(obj[kAppIdKey].GetString());
  }
  if (obj.HasMember(kCursorThemeKey) && obj[kCursorThemeKey].IsString()) {
    instance.cursor_theme.assign(obj[kCursorThemeKey].GetString());
  }
  if (obj.HasMember(kDisableCursorKey) && obj[kDisableCursorKey].IsBool()) {
    instance.disable_cursor = obj[kDisableCursorKey].GetBool();
  }
  if (obj.HasMember(kDebugBackendKey) && obj[kDebugBackendKey].IsBool()) {
    instance.debug_backend = obj[kDebugBackendKey].GetBool();
  }

  if (obj.HasMember(kCommandLineArgsKey)) {
    if (obj[kCommandLineArgsKey].IsArray()) {
      auto args = obj[kCommandLineArgsKey].GetArray();
      instance.view.command_line_args.reserve(args.Capacity());
      for (auto const& arg : args) {
        instance.view.command_line_args.emplace_back(arg.GetString());
      }
    } else {
      FML_LOG(ERROR)
          << "Command Line args in JSON config has to be an array of strings";
      exit(EXIT_FAILURE);
    }
  }

  if (obj.HasMember(kBundlePathKey) && obj[kBundlePathKey].IsString()) {
    instance.view.bundle_path.assign(obj[kBundlePathKey].GetString());
  }
  if (obj.HasMember(kWindowTypeKey) && obj[kWindowTypeKey].IsString()) {
    instance.view.window_type.assign(obj[kWindowTypeKey].GetString());
  }
  if (obj.HasMember(kAccessibilityFeaturesKey) &&
      obj[kAccessibilityFeaturesKey].IsInt()) {
    instance.view.accessibility_features =
        obj[kAccessibilityFeaturesKey].GetInt();
  }
  if (obj.HasMember(kWidthKey) && obj[kWidthKey].IsInt()) {
    instance.view.width = obj[kWidthKey].GetInt();
  }
  if (obj.HasMember(kHeightKey) && obj[kHeightKey].IsInt()) {
    instance.view.height = obj[kHeightKey].GetInt();
  }
  if (obj.HasMember(kFullscreenKey) && obj[kFullscreenKey].IsBool()) {
    instance.view.fullscreen = obj[kFullscreenKey].GetBool();
  }
}

void Configuration::getView(rapidjson::Document& doc,
                            int index,
                            Config& instance) {
  if (doc[kViewKey].IsArray()) {
    auto arr = doc[kViewKey].GetArray();
    if (index > arr.Capacity())
      assert(false);

    getViewParameters(arr[index].GetObject(), instance);
    getGlobalParameters(doc.GetObject(), instance);
  } else {
    getViewParameters(doc[kViewKey].GetObject(), instance);
    getGlobalParameters(doc.GetObject(), instance);
  }
}

void Configuration::getCliOverrides(rapidjson::Document& doc,
                                    Config& instance,
                                    Config& cli) {
  if (!cli.app_id.empty()) {
    instance.app_id.assign(cli.app_id);
  }
  if (!cli.json_configuration_path.empty()) {
    instance.json_configuration_path.assign(cli.json_configuration_path);
  }
  if (!cli.cursor_theme.empty()) {
    instance.cursor_theme.assign(cli.cursor_theme);
  }
  if (cli.disable_cursor != instance.disable_cursor) {
    instance.disable_cursor = cli.disable_cursor;
  }
  if (cli.debug_backend != instance.debug_backend) {
    instance.debug_backend = cli.debug_backend;
  }

  if (!cli.view.command_line_args.empty()) {
    instance.view.command_line_args.clear();
    instance.view.command_line_args.reserve(
        cli.view.command_line_args.capacity());
    for (auto const& arg : instance.view.command_line_args) {
      instance.view.command_line_args.emplace_back(arg);
    }
  }

  if (!cli.view.bundle_path.empty()) {
    instance.view.bundle_path.assign(cli.view.bundle_path);
  }
  if (!cli.view.window_type.empty()) {
    instance.view.window_type.assign(cli.view.window_type);
  }
  if (cli.view.accessibility_features > 0) {
    instance.view.accessibility_features = cli.view.accessibility_features;
  }
  if (cli.view.width > 0) {
    instance.view.width = cli.view.width;
  }
  if (cli.view.height > 0) {
    instance.view.height = cli.view.height;
  }
  if (cli.view.fullscreen != instance.view.fullscreen) {
    instance.view.fullscreen = cli.view.fullscreen;
  }
}

std::vector<struct Configuration::Config> Configuration::ParseConfig(
    Configuration::Config& config) {
  rapidjson::Document doc;
  if (!config.json_configuration_path.empty()) {
    doc = getJsonDocument(config.json_configuration_path);
    Validate(doc);
  }

  auto view_count = getViewCount(doc);
  FML_DLOG(INFO) << "View Count: " << view_count;
  std::vector<struct Config> res;
  res.reserve(view_count);
  for (int i = 0; i < view_count; i++) {
    Config cfg{};
    getView(doc, i, cfg);
    getCliOverrides(doc, cfg, config);

    if (cfg.view.window_type.empty())
      cfg.view.window_type.assign("NORMAL");

    if (cfg.view.bundle_path.empty()) {
      FML_LOG(ERROR) << "A bundle path must be specified";
      exit(EXIT_FAILURE);
    }
    if (cfg.view.width == 0) {
      FML_LOG(ERROR) << "A view width must be specified";
      exit(EXIT_FAILURE);
    }
    if (cfg.view.height == 0) {
      FML_LOG(ERROR) << "A view height must be specified";
      exit(EXIT_FAILURE);
    }

    res.emplace_back(cfg);
  }
  assert(res.capacity() == view_count);

  return res;
}

void Configuration::Validate(rapidjson::Document& doc) {
  std::string missingParams;
  if (!doc.HasMember(kViewKey))
    missingParams += "view ";

  if (!missingParams.empty()) {
    FML_LOG(ERROR) << "Missing parameter(s) in JSON: " << missingParams;
    exit(EXIT_FAILURE);
  }
}

rapidjson::Document Configuration::getJsonDocument(
    const std::string& filename) {
  std::ifstream json_file(filename);
  if (!json_file.is_open()) {
    FML_LOG(ERROR) << "Unable to open file " << filename;
    exit(EXIT_FAILURE);
  }

  std::stringstream contents;
  contents << json_file.rdbuf();

  rapidjson::Document doc;
  doc.Parse(contents.str().c_str());
  return doc;
}

void Configuration::PrintConfig(const Config& config) {
  FML_LOG(INFO) << "**********";
  FML_LOG(INFO) << "* Global *";
  FML_LOG(INFO) << "**********";
  FML_LOG(INFO) << "Application Id: .......... " << config.app_id;
  FML_LOG(INFO) << "JSON Configuration: ...... "
                << config.json_configuration_path;
  FML_LOG(INFO) << "Cursor Theme: ............ " << config.cursor_theme;
  FML_LOG(INFO) << "Disable Cursor: .......... "
                << (config.disable_cursor ? "true" : "false");
  FML_LOG(INFO) << "Debug Backend: ........... "
                << (config.debug_backend ? "true" : "false");
  FML_LOG(INFO) << "********";
  FML_LOG(INFO) << "* View *";
  FML_LOG(INFO) << "********";
  FML_LOG(INFO) << "Command Line Args:";
  for (auto const& arg : config.view.command_line_args) {
    FML_LOG(INFO) << arg;
  }
  FML_LOG(INFO) << "Bundle Path: .............. " << config.view.bundle_path;
  FML_LOG(INFO) << "Window Type: .............. " << config.view.window_type;
  FML_LOG(INFO) << "Accessibility Features: ... "
                << config.view.accessibility_features;
  FML_LOG(INFO) << "Width: .................... " << config.view.width;
  FML_LOG(INFO) << "Height: ................... " << config.view.height;
  FML_LOG(INFO) << "Fullscreen: ............... "
                << (config.view.fullscreen ? "true" : "false");
}
