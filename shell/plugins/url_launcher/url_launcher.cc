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

#include "url_launcher.h"

#include <sys/wait.h>
#include <unistd.h>

#include <flutter/standard_method_codec.h>

#include "engine.h"
#include "logging.h"

void UrlLauncher::OnPlatformMessage(const FlutterPlatformMessage* message,
                                    void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  const auto engine = static_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  const auto obj =
      codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();

  if (method == kLaunchMethod) {
    if (!obj->arguments()->IsNull()) {
      const auto args = std::get_if<flutter::EncodableMap>(obj->arguments());

      std::string url;
      const auto it = args->find(flutter::EncodableValue(kUrlKey));
      if (it != args->end() && !it->second.IsNull()) {
        url = std::get<std::string>(it->second);
      }

      const pid_t pid = fork();
      if (pid == 0) {
        execl("/usr/bin/xdg-open", "xdg-open", url.c_str(), nullptr);
        exit(1);
      }
      int status = 0;
      waitpid(pid, &status, 0);
      if (status != 0) {
        std::ostringstream error_message;
        error_message << "Failed to open " << url << ": error " << status;
        result = codec.EncodeErrorEnvelope(kLaunchError, error_message.str());
        engine->SendPlatformMessageResponse(message->response_handle,
                                            result->data(), result->size());
        return;
      }
      const auto val = flutter::EncodableValue(true);
      result = codec.EncodeSuccessEnvelope(&val);
    } else {
      result = codec.EncodeErrorEnvelope("argument_error", "Invalid Arguments");
    }
  } else if (method == kCanLaunchMethod) {
    if (!obj->arguments()->IsNull()) {
      const auto args = std::get_if<flutter::EncodableMap>(obj->arguments());

      const auto it = args->find(flutter::EncodableValue(kUrlKey));
      if (it != args->end() && !it->second.IsNull()) {
        const std::string url = std::get<std::string>(it->second);
        const flutter::EncodableValue response(
            (url.rfind("https:", 0) == 0) || (url.rfind("http:", 0) == 0) ||
            (url.rfind("ftp:", 0) == 0) || (url.rfind("file:", 0) == 0));
        result = codec.EncodeSuccessEnvelope(&response);
      } else {
        result = codec.EncodeErrorEnvelope("argument_error", "No URL provided");
      }
    } else {
      result = codec.EncodeErrorEnvelope("argument_error", "Invalid Arguments");
    }
  } else {
    SPDLOG_DEBUG("url_launcher: {} is unhandled", method);
    result = codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  }
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
