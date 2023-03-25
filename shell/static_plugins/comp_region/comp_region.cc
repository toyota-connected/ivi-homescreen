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

#include "comp_region.h"

#include "../../view/flutter_view.h"
#include "engine.h"

void CompositorRegionPlugin::ClearGroups(flutter::EncodableList& types,
                                         FlutterView* view) {
  for (auto const& encoded_types : types) {
    std::string type = std::get<std::string>(encoded_types);
    view->ClearRegion(type);
  }
}

flutter::EncodableValue CompositorRegionPlugin::HandleGroups(
    flutter::EncodableList& groups,
    FlutterView* view) {
  flutter::EncodableList results;

  for (auto const& group : groups) {
    auto arguments = std::get<flutter::EncodableMap>(group);

    std::string type;
    auto it = arguments.find(flutter::EncodableValue(kArgGroupType));
    if (it != arguments.end()) {
      type = std::get<std::string>(it->second);
    }

    flutter::EncodableList encoded_regions;
    it = arguments.find(flutter::EncodableValue(kArgRegions));
    if (it != arguments.end()) {
      encoded_regions = std::get<flutter::EncodableList>(it->second);
    }

    if (!encoded_regions.empty()) {
      std::vector<REGION_T> regions;
      for (auto const& region : encoded_regions) {
        auto args = std::get<flutter::EncodableMap>(region);

        auto x = 0;
        it = args.find(flutter::EncodableValue(kArgRegionX));
        if (it != args.end()) {
          x = std::get<int32_t>(it->second);
        }

        auto y = 0;
        it = args.find(flutter::EncodableValue(kArgRegionY));
        if (it != args.end()) {
          y = std::get<int32_t>(it->second);
        }

        auto width = 0;
        it = args.find(flutter::EncodableValue(kArgRegionWidth));
        if (it != args.end()) {
          width = std::get<int>(it->second);
        }

        auto height = 0;
        it = args.find(flutter::EncodableValue(kArgRegionHeight));
        if (it != args.end()) {
          height = std::get<int>(it->second);
        }

        regions.push_back({x, y, width, height});
      }
      view->SetRegion(type, regions);
      results.emplace_back(type);
    }
  }

  return flutter::EncodableValue(results);
}

void CompositorRegionPlugin::OnPlatformMessage(
    const FlutterPlatformMessage* message,
    void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();

  if (method == kMethodMask) {
    if (!obj->arguments()->IsNull()) {
      auto args = std::get_if<flutter::EncodableMap>(obj->arguments());

      /* Clear array */
      auto it = args->find(flutter::EncodableValue(kArgClear));
      if (it != args->end()) {
        auto types = std::get<flutter::EncodableList>(it->second);
        ClearGroups(types, engine->GetView());
      }

      /* Group array */
      flutter::EncodableValue value;
      it = args->find(flutter::EncodableValue(kArgGroups));
      if (it != args->end()) {
        auto groups = std::get<flutter::EncodableList>(it->second);
        value = HandleGroups(groups, engine->GetView());
      }

      result = codec.EncodeSuccessEnvelope(&value);

    } else {
      result = codec.EncodeErrorEnvelope("argument_error", "Invalid Arguments");
    }
  }
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
