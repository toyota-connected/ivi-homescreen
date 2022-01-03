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

#include "connectivity.h"

#include <flutter/fml/logging.h>
#include <flutter/shell/platform/common/client_wrapper/include/flutter/standard_method_codec.h>

#include "engine.h"

void Connectivity::OnPlatformMessage(const FlutterPlatformMessage* message,
                                     void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  std::unique_ptr<std::vector<std::uint8_t>> result;
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj =
      codec.DecodeMethodCall(message->message, message->message_size);

  auto method_name = obj->method_name();

#if 0
  wifi
  mobile
  none
#endif

  if (method_name == "check") {
    flutter::EncodableValue val("wifi");
    result = codec.EncodeSuccessEnvelope(&val);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        result->data(), result->size());
    return;
  } else if (method_name == "wifiName") {
    FML_DLOG(INFO) << "wifiName";
  } else if (method_name == "wifiBSSID") {
    FML_DLOG(INFO) << "wifiBSSID";
  } else if (method_name == "wifiIPAddress") {
    FML_DLOG(INFO) << "wifiIPAddress";
  } else if (method_name == "requestLocationServiceAuthorization") {
    FML_DLOG(INFO) << "requestLocationServiceAuthorization";
  } else if (method_name == "getLocationServiceAuthorization") {
    FML_DLOG(INFO) << "getLocationServiceAuthorization";
#if 0
  notDetermined
  restricted
  denied
  authorizedAlways
  authorizedWhenInUse
#endif
  }
}

void Connectivity::OnPlatformMessageStatus(
    const FlutterPlatformMessage* message,
    void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  std::unique_ptr<std::vector<std::uint8_t>> result;
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj =
      codec.DecodeMethodCall(message->message, message->message_size);
  FML_DLOG(INFO) << "ConnectivityStatus: " << obj->method_name();

  flutter::EncodableValue val(true);
  result = codec.EncodeSuccessEnvelope(&val);
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
