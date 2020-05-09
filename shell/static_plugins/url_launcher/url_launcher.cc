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
#include <flutter/standard_method_codec.h>

#include "engine.h"

void UrlLauncher::OnPlatformMessage(const FlutterPlatformMessage* message,
                                    void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  std::unique_ptr<std::vector<std::uint8_t>> result;
  auto codec = &flutter::StandardMethodCodec::GetInstance();
  auto method_call =
      codec->DecodeMethodCall(message->message, message->message_size);

  if (method_call->method_name() == "launch") {
    std::string url;
    if (method_call->arguments() && method_call->arguments()->IsMap()) {
      const flutter::EncodableMap& arguments =
          method_call->arguments()->MapValue();
      auto url_it = arguments.find(flutter::EncodableValue("url"));
      if (url_it != arguments.end()) {
        url = url_it->second.StringValue();
      }
    }
    if (url.empty()) {
      result = codec->EncodeErrorEnvelope("argument_error", "No URL provided");
      goto done;
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
      result = codec->EncodeErrorEnvelope("open_error", error_message.str());
      goto done;
    }
    auto val = flutter::EncodableValue(true);
    result = codec->EncodeSuccessEnvelope(&val);

  } else if (method_call->method_name() == "canLaunch") {
    std::string url;
    if (method_call->arguments() && method_call->arguments()->IsMap()) {
      const flutter::EncodableMap& arguments =
          method_call->arguments()->MapValue();
      auto url_it = arguments.find(flutter::EncodableValue("url"));
      if (url_it != arguments.end()) {
        url = url_it->second.StringValue();
      }
    }
    if (url.empty()) {
      result = codec->EncodeErrorEnvelope("argument_error", "No URL provided");
      goto done;
    }
    flutter::EncodableValue response(
        (url.rfind("https:", 0) == 0) || (url.rfind("http:", 0) == 0) ||
        (url.rfind("ftp:", 0) == 0) || (url.rfind("file:", 0) == 0));
    result = codec->EncodeSuccessEnvelope(&response);
  }
done:
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
