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

#include "navigation.h"

#include <flutter/fml/logging.h>
#include <flutter/shell/platform/common/json_method_codec.h>

#include "engine.h"

void Navigation::OnPlatformMessage(const FlutterPlatformMessage *message,
                                   void *userdata) {
    std::unique_ptr<std::vector<uint8_t>> result;
    auto engine = reinterpret_cast<Engine *>(userdata);
    auto &codec = flutter::JsonMethodCodec::GetInstance();
    auto obj = codec.DecodeMethodCall(message->message, message->message_size);

    auto method = obj->method_name();

    if (method == kSelectSingleEntryHistory) {
        if (obj->arguments()->IsNull()) {
            FML_LOG(INFO) << "Navigation: Select Single Entry History";
            result = codec.EncodeSuccessEnvelope();
        } else {
            result = codec.EncodeErrorEnvelope("argument_error", "Invalid Arguments");
        }
    } else if (method == kRouteInformationUpdated) {
        auto args = obj->arguments();
        if (!args->IsNull() && args->HasMember("location") &&
            args->HasMember("state") && args->HasMember("replace")) {
            RouteInformation info{};
            info.location = (*args)["location"].GetString();
            info.state =
                    !(*args)["state"].IsNull() ? (*args)["state"].GetString() : "";
            info.replace = (*args)["replace"].GetBool();
            FML_LOG(INFO) << "Navigation: Route Information Updated"
                             "\n\tlocation: "
                          << info.location << "\n\tstate: " << info.state
                          << "\n\treplace: " << info.replace;
            result = codec.EncodeSuccessEnvelope();
        } else {
            result = codec.EncodeErrorEnvelope("argument_error", "Invalid Arguments");
        }
    } else {
        FML_DLOG(INFO) << "Navigation: " << method << " is unhandled";
        result = codec.EncodeErrorEnvelope("unhandled_method", "unhandled Method");
    }

    engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                        result->size());
}
