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

#include "accessibility.h"

#include <flutter/fml/logging.h>
#include <flutter/standard_message_codec.h>

#include "engine.h"

void Accessibility::OnPlatformMessage(const FlutterPlatformMessage *message,
                                      void *userdata) {
    auto engine = reinterpret_cast<Engine *>(userdata);
    auto &codec = flutter::StandardMessageCodec::GetInstance();
    auto obj = codec.DecodeMessage(message->message, message->message_size);

    if (!obj->IsNull()) {
        auto map = std::get<flutter::EncodableMap>(*obj);
        auto type = std::get<std::string>(map[flutter::EncodableValue("type")]);
        auto data =
                std::get<flutter::EncodableMap>(map[flutter::EncodableValue("data")]);
        auto msg = std::get<std::string>(data[flutter::EncodableValue("message")]);

        FML_DLOG(INFO) << "Accessibility: type: " << type << ", message: " << msg;

        if (type == "disableAnimations") {
            auto value = engine->GetAccessibilityFeatures();
            auto enabled = std::get<bool>(data[flutter::EncodableValue("enabled")]);
            if (enabled) {
                value |= FlutterAccessibilityFeature::kFlutterAccessibilityFeatureDisableAnimations;
            }
            else {
                value &= ~FlutterAccessibilityFeature::kFlutterAccessibilityFeatureDisableAnimations;
            }
            engine->UpdateAccessibilityFeatures(value);
            FML_DLOG(INFO) << "Accessibility: type: " << type << ", enabled: " << (enabled ? "true" : "false");
        }
    }

    engine->SendPlatformMessageResponse(message->response_handle, nullptr, 0);
}
