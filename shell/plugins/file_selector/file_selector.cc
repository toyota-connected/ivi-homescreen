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

#include "file_selector.h"

#include <filesystem>

#include <flutter/standard_method_codec.h>

#include "engine.h"
#include "utils.h"

void FileSelector::OnPlatformMessage(const FlutterPlatformMessage* message,
                                     void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();

  if (method == kGetDirectoryPath) {
    SPDLOG_DEBUG("[file_selector] getDirectoryPath:");
    if (obj->arguments()->IsNull()) {
      result = codec.EncodeErrorEnvelope("invalid_arguments", "");
      engine->SendPlatformMessageResponse(message->response_handle,
                                          result->data(), result->size());
      return;
    }

    std::string initialDirectory;
    std::string confirmButtonText;
    bool multiple{};
    std::ostringstream oss;

    auto args = std::get_if<flutter::EncodableMap>(obj->arguments());
    for (auto& it : *args) {
      auto key = std::get<std::string>(it.first);
      if (key == kArgInitialDirectory && !it.second.IsNull() &&
          std::holds_alternative<std::string>(it.second)) {
        initialDirectory = std::get<std::string>(it.second);
      } else if (key == kArgConfirmButtonText && !it.second.IsNull()) {
        confirmButtonText.assign(std::get<std::string>(it.second));
      } else if (key == kArgMultiple && !it.second.IsNull() &&
                 std::holds_alternative<bool>(it.second)) {
        multiple = std::get<bool>(it.second);
      }
    }

    SPDLOG_DEBUG("initialDirectory: [{}]", initialDirectory);
    if (!initialDirectory.empty()) {
      if (std::filesystem::exists(initialDirectory)) {
        oss << "cd " << initialDirectory << " && ";
      }
    }

    oss << "zenity --file-selection --directory";

    SPDLOG_DEBUG("multiple: [{}]", multiple);
    if (multiple) {
      oss << " --multiple";
    }

    SPDLOG_DEBUG("confirmButtonText: [{}]", confirmButtonText);
    if (!confirmButtonText.empty()) {
      oss << " --title=" << confirmButtonText;
    }

    SPDLOG_DEBUG("cmd: [{}]", oss.str());

    char path[PATH_MAX];
    if (!Utils::ExecuteCommand(oss.str().c_str(), path)) {
      result = codec.EncodeErrorEnvelope("failed", "failed to execute command");
      engine->SendPlatformMessageResponse(message->response_handle,
                                          result->data(), result->size());
      return;
    }
    flutter::EncodableList results;
    auto paths = Utils::split(path, "|");
    for (auto p : paths) {
      results.emplace_back(std::move(Utils::trim(p, "\n")));
    }
    flutter::EncodableValue val(results);
    result = codec.EncodeSuccessEnvelope(&val);

  } else if (method == kGetSavePath) {
    SPDLOG_DEBUG("[file_selector] getSavePath:");
    if (obj->arguments()->IsNull()) {
      result = codec.EncodeErrorEnvelope("invalid_arguments", "");
      engine->SendPlatformMessageResponse(message->response_handle,
                                          result->data(), result->size());
      return;
    }

    std::filesystem::path filepath;
    std::string initialDirectory;
    std::string suggestedName;
    std::string confirmButtonText;
    std::ostringstream oss;

    auto args = std::get_if<flutter::EncodableMap>(obj->arguments());
    for (auto& it : *args) {
      auto key = std::get<std::string>(it.first);
      if (key == kArgInitialDirectory && !it.second.IsNull() &&
          std::holds_alternative<std::string>(it.second)) {
        initialDirectory = std::get<std::string>(it.second);
      } else if (key == kArgSuggestedName && !it.second.IsNull() &&
                 std::holds_alternative<std::string>(it.second)) {
        suggestedName.assign(std::get<std::string>(it.second));
      } else if (key == kArgConfirmButtonText && !it.second.IsNull()) {
        confirmButtonText.assign(std::get<std::string>(it.second));
      }
    }

    SPDLOG_DEBUG("initialDirectory: [{}]", initialDirectory);
    filepath.assign(initialDirectory);
    if (!exists(filepath)) {
      if (!std::filesystem::create_directories(filepath)) {
        result =
            codec.EncodeErrorEnvelope("error", "Failed to create directories");
        engine->SendPlatformMessageResponse(message->response_handle,
                                            result->data(), result->size());
        return;
      }
    }
    SPDLOG_DEBUG("suggestedName: [{}]", suggestedName);
    filepath.append(suggestedName);
    SPDLOG_DEBUG("confirmButtonText: [{}]", confirmButtonText);
    SPDLOG_DEBUG("save filepath: [{}]", filepath.c_str());
    flutter::EncodableValue val(filepath.c_str());
    result = codec.EncodeSuccessEnvelope(&val);
  } else if (method == kMethodOpenFile) {
    SPDLOG_DEBUG("[file_selector] openFile");

    if (obj->arguments()->IsNull()) {
      result = codec.EncodeErrorEnvelope("invalid_arguments", "");
      engine->SendPlatformMessageResponse(message->response_handle,
                                          result->data(), result->size());
      return;
    }

    std::string initialDirectory;
    std::string confirmButtonText;
    std::string label;
    std::ostringstream oss;
    std::stringstream extensions;
    bool multiple{};

    auto args = std::get_if<flutter::EncodableMap>(obj->arguments());
    for (auto& it : *args) {
      auto key = std::get<std::string>(it.first);
      if (key == kArgAcceptedTypeGroups && !it.second.IsNull() &&
          std::holds_alternative<flutter::EncodableList>(it.second)) {
        auto acceptedTypeGroups = std::get<flutter::EncodableList>(it.second);
        for (auto const& group : acceptedTypeGroups) {
          std::map<flutter::EncodableValue, flutter::EncodableValue> map;
          if (!group.IsNull() &&
              std::holds_alternative<flutter::EncodableMap>(group)) {
            map = std::get<flutter::EncodableMap>(group);
            for (const auto& ele : map) {
              if (std::holds_alternative<std::string>(ele.first) &&
                  std::holds_alternative<std::string>(ele.second)) {
                auto k = std::get<std::string>(ele.first);
                if (k == kArgTypeGroupLabel) {
                  label = std::get<std::string>(ele.second);
                  SPDLOG_DEBUG("{}: {}", k, label);
                }
              }
              if (std::holds_alternative<std::string>(ele.first) &&
                  std::holds_alternative<flutter::EncodableList>(ele.second)) {
                auto k = std::get<std::string>(ele.first);
                auto list = std::get<flutter::EncodableList>(ele.second);
                for (const auto& item : list) {
                  if (std::holds_alternative<std::string>(item)) {
                    auto value = std::get<std::string>(item);
                    if (k == kArgTypeGroupExtensions) {
                      extensions << " " << value;
                      SPDLOG_DEBUG("{}: {}", k, value);
                    }
                  }
                }
              }
            }
          }
        }
      } else if (key == kArgInitialDirectory && !it.second.IsNull() &&
                 std::holds_alternative<std::string>(it.second)) {
        initialDirectory.assign(std::get<std::string>(it.second));
      } else if (key == kArgConfirmButtonText && !it.second.IsNull()) {
        confirmButtonText.assign(std::get<std::string>(it.second));
      } else if (key == kArgMultiple && !it.second.IsNull() &&
                 std::holds_alternative<bool>(it.second)) {
        multiple = std::get<bool>(it.second);
      }
    }

    SPDLOG_DEBUG("initialDirectory: [{}]", initialDirectory);
    if (!initialDirectory.empty()) {
      if (std::filesystem::exists(initialDirectory)) {
        oss << "cd " << initialDirectory << " && ";
      }
    }

    oss << "zenity --file-selection --file-filter=\"" << label << " | "
        << extensions.str() << "\"";

    SPDLOG_DEBUG("multiple: {}", multiple);
    if (multiple) {
      oss << " --multiple";
    }

    SPDLOG_DEBUG("confirmButtonText: [{}]", confirmButtonText);
    if (!confirmButtonText.empty()) {
      oss << " --title=" << confirmButtonText;
    }

    SPDLOG_DEBUG("cmd: [{}]", oss.str());

    char path[PATH_MAX];
    if (!Utils::ExecuteCommand(oss.str().c_str(), path)) {
      result = codec.EncodeErrorEnvelope("failed", "failed to execute command");
      engine->SendPlatformMessageResponse(message->response_handle,
                                          result->data(), result->size());
      return;
    }

    flutter::EncodableList results;
    auto paths = Utils::split(path, "|");
    for (auto p : paths) {
      results.emplace_back(std::move(Utils::trim(p, "\n")));
    }
    flutter::EncodableValue val(results);
    result = codec.EncodeSuccessEnvelope(&val);
  } else {
    result = codec.EncodeErrorEnvelope("error", "method not handled");
  }

  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
