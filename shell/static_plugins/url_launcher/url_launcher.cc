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

#include <flutter/fml/logging.h>
#include <flutter/shell/platform/common/client_wrapper/include/flutter/standard_method_codec.h>

#include "engine.h"

void UrlLauncher::OnPlatformMessage(const FlutterPlatformMessage* message,
                                    void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  std::unique_ptr<std::vector<std::uint8_t>> result;
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj =
      codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();

  if (method == "launch") {
    if (!obj->arguments()->IsNull()) {
      auto args = std::get_if<flutter::EncodableMap>(obj->arguments());

      std::string url;
      auto it = args->find(flutter::EncodableValue("url"));
      if (it != args->end()) {
        url = std::get<std::string>(it->second);
      }
      else {
        result = codec.EncodeErrorEnvelope("argument_error", "No URL provided");
        engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                            result->size());
        return;
      }

      pid_t pid = fork();
      if (pid == 0) {
        execl("/usr/bin/xdg-open", "xdg-open", url.c_str(), nullptr);
        exit(1);
      }
      int status = 0;
      waitpid(pid, &status, 0);
      if (status != 0) {
        std::ostringstream error_message;
        error_message << "Failed to open " << url << ": error " << status;
        result = codec.EncodeErrorEnvelope("open_error", error_message.str());
        engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                            result->size());
        return;
      }
      auto val = flutter::EncodableValue(true);
      result = codec.EncodeSuccessEnvelope(&val);
      engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                          result->size());
    }
  } else if (method == "canLaunch") {
    std::string url;
    if (!obj->arguments()->IsNull()) {
      auto args = std::get_if<flutter::EncodableMap>(obj->arguments());

      auto it = args->find(flutter::EncodableValue("url"));
      if (it != args->end()) {
        url = std::get<std::string>(it->second);
      }
      else {
        result = codec.EncodeErrorEnvelope("argument_error", "No URL provided");
        engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                            result->size());
        return;
      }
    }
    flutter::EncodableValue response(
        (url.rfind("https:", 0) == 0) || (url.rfind("http:", 0) == 0) ||
        (url.rfind("ftp:", 0) == 0) || (url.rfind("file:", 0) == 0));
    result = codec.EncodeSuccessEnvelope(&response);
    engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                        result->size());
    return;
  }
}
