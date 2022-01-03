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


#include "isolate.h"

#include <flutter/fml/logging.h>
#include <flutter/shell/platform/common/client_wrapper/include/flutter/standard_method_codec.h>

#include "engine.h"

void Isolate::OnPlatformMessage(const FlutterPlatformMessage* message,
                                void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);

  std::string msg;
  msg.append(reinterpret_cast<const char*>(message->message));
  msg.resize(message->message_size);
  FML_DLOG(INFO) << "Root Isolate Service ID: \"" << message->message << "\"";

  auto res = flutter::StandardMethodCodec::GetInstance().EncodeSuccessEnvelope();
  engine->SendPlatformMessageResponse(message->response_handle, res->data(), res->size());
}
