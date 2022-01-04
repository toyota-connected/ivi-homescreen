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
  std::unique_ptr<std::vector<uint8_t>> result;
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();
  if (method == "check") {
    FML_DLOG(INFO) << "check";
    // wifi, mobile, none
    flutter::EncodableValue val("none");
    result = codec.EncodeSuccessEnvelope(&val);
  } else if (method == "wifiName") {
    FML_DLOG(INFO) << "wifiName";
    flutter::EncodableValue val("wlan0");
    result = codec.EncodeSuccessEnvelope(&val);
  } else if (method == "wifiBSSID") {
    FML_DLOG(INFO) << "wifiBSSID";
    flutter::EncodableValue val("SSID");
    result = codec.EncodeSuccessEnvelope(&val);
  } else if (method == "wifiIPAddress") {
    FML_DLOG(INFO) << "wifiIPAddress";
    flutter::EncodableValue val("127.0.0.1");
    result = codec.EncodeSuccessEnvelope(&val);
  } else if (method == "requestLocationServiceAuthorization") {
    FML_DLOG(INFO) << "requestLocationServiceAuthorization";
    result = codec.EncodeSuccessEnvelope();
  } else if (method == "getLocationServiceAuthorization") {
    FML_DLOG(INFO) << "getLocationServiceAuthorization";
    // notDetermined, restricted, denied, authorizedAlways, authorizedWhenInUse
    flutter::EncodableValue val("denied");
    result = codec.EncodeSuccessEnvelope(&val);
  } else {
    result = codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  }

  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}

void Connectivity::OnPlatformMessageStatus(
    const FlutterPlatformMessage* message,
    void* userdata) {
  auto& engine = reinterpret_cast<Engine&>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  FML_DLOG(INFO) << "ConnectivityStatus: " << obj->method_name();

  flutter::EncodableValue val(true);
  auto result = codec.EncodeSuccessEnvelope(&val);
  engine.SendPlatformMessageResponse(message->response_handle, result->data(),
                                     result->size());
}
