/*
* Copyright 2023 Toyota Connected North America
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

#include "integration_test.h"

#include <flutter/standard_method_codec.h>
#include <thread>
#include <chrono>

#include "engine.h"

using namespace std::chrono_literals;

void IntegrationTestPlugin::OnPlatformMessage(
    const FlutterPlatformMessage* message,
    void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  std::unique_ptr<std::vector<uint8_t>> result =
      codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  do {
    auto method = obj->method_name();
    if (method == kAllTestsFinished) {
      if (!obj->arguments()->IsNull()) {
        auto args = std::get_if<flutter::EncodableMap>(obj->arguments());
        if (args == nullptr) {
          result = codec.EncodeErrorEnvelope("argument_error",
                                             "no arguments provided");
          break;
        }
        for (const auto& it : *args) {
          auto key = std::get<std::string>(it.first);
          if (key == kArgResults) {
            if (!it.second.IsNull()) {
              auto value = std::get<flutter::EncodableMap>(it.second);
              for (const auto& results : value) {
                auto v = std::get<std::string>(results.second);
                result = codec.EncodeSuccessEnvelope();
                engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                                    result->size());
                // ensure platform channel drains
                std::this_thread::sleep_for(1000ms);
                if (v == kResultSuccess) {
                  std::exit(0);
                } else {
                  std::exit(1);
                }
              }
            }
          }
        }
      }
    } else if (method == kConvertFlutterSurfaceToImage) {
      result = codec.EncodeErrorEnvelope("Could not convert to image",
                                         "Not implemented yet");
    } else if (method == kRevertFlutterImage) {
      result = codec.EncodeErrorEnvelope("Could not revert Flutter image",
                                         "Not implemented yet");
    } else if (method == kCaptureScreenshot) {
      result = codec.EncodeErrorEnvelope("Could not capture screenshot",
                                         "Not implemented yet");
    } else if (method == kScheduleFrame) {
      result = codec.EncodeErrorEnvelope("Could not schedule frame",
                                         "Not implemented yet");
    }
  } while (false);

  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
