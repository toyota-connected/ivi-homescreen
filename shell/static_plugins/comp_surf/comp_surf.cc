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

#include "comp_surf.h"

#include <dlfcn.h>
#include <flutter/fml/logging.h>
#include <flutter/standard_method_codec.h>

#include "../../view/flutter_view.h"
#include "engine.h"

void CompositorSurfacePlugin::OnPlatformMessage(
    const FlutterPlatformMessage* message,
    void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();

  if (method == kMethodCreate) {
    if (!obj->arguments()->IsNull()) {
      auto args = std::get_if<flutter::EncodableMap>(obj->arguments());

      int64_t viewId = 0;
      auto it = args->find(flutter::EncodableValue(kArgView));
      if (it != args->end()) {
        flutter::EncodableValue encodedValue = it->second;
        viewId = encodedValue.LongValue();
      }

      auto view = engine->GetView();
      if (viewId != view->GetIndex()) {
        assert(false);
      }

      std::string module_str;
      it = args->find(flutter::EncodableValue(kArgModule));
      if (it != args->end()) {
        module_str = std::get<std::string>(it->second);
      }
      auto h_module = dlopen(module_str.c_str(), RTLD_LAZY);
      if (!h_module) {
        result = codec.EncodeErrorEnvelope("module_error", "not found");
        engine->SendPlatformMessageResponse(message->response_handle,
                                            result->data(), result->size());
        return;
      }

      std::string assets_path;
      it = args->find(flutter::EncodableValue(kArgAssetsPath));
      if (it != args->end()) {
        assets_path = std::get<std::string>(it->second);
      }

      std::string cache_folder;
      it = args->find(flutter::EncodableValue(kCacheFolder));
      if (it != args->end()) {
        cache_folder = std::get<std::string>(it->second);
      }

      std::string type_str;
      it = args->find(flutter::EncodableValue(kArgType));
      if (it != args->end()) {
        type_str = std::get<std::string>(it->second);
      }
      CompositorSurface::PARAM_SURFACE_T type;
      if (type_str == kParamTypeEgl) {
        type = CompositorSurface::PARAM_SURFACE_T::egl;
      } else if (type_str == kParamTypeVulkan) {
        type = CompositorSurface::PARAM_SURFACE_T::vulkan;
      } else {
        dlclose(h_module);
        result = codec.EncodeErrorEnvelope("type_error", "value invalid");
        engine->SendPlatformMessageResponse(message->response_handle,
                                            result->data(), result->size());
        return;
      }

      std::string z_order_str;
      it = args->find(flutter::EncodableValue(kArgZOrder));
      if (it != args->end()) {
        z_order_str = std::get<std::string>(it->second);
      }
      CompositorSurface::PARAM_Z_ORDER_T z_order;
      if (z_order_str == kParamZOrderAbove) {
        z_order = CompositorSurface::PARAM_Z_ORDER_T::above;
      } else if (z_order_str == kParamZOrderBelow) {
        z_order = CompositorSurface::PARAM_Z_ORDER_T::below;
      } else {
        result = codec.EncodeErrorEnvelope("z_order_error", "value invalid");
        engine->SendPlatformMessageResponse(message->response_handle,
                                            result->data(), result->size());
        return;
      }

      std::string sync_str;
      it = args->find(flutter::EncodableValue(kArgSync));
      if (it != args->end()) {
        sync_str = std::get<std::string>(it->second);
      }
      CompositorSurface::PARAM_SYNC_T sync;
      if (sync_str == kParamSyncSync) {
        sync = CompositorSurface::PARAM_SYNC_T::sync;
      } else if (sync_str == kParamSyncDeSync) {
        sync = CompositorSurface::PARAM_SYNC_T::de_sync;
      } else {
        dlclose(h_module);
        result = codec.EncodeErrorEnvelope("sync_error", "value invalid");
        engine->SendPlatformMessageResponse(message->response_handle,
                                            result->data(), result->size());
        return;
      }

      auto width = kDefaultViewWidth;
      it = args->find(flutter::EncodableValue(kArgWidth));
      if (it != args->end()) {
        width = std::get<int>(it->second);
      }

      auto height = kDefaultViewHeight;
      it = args->find(flutter::EncodableValue(kArgHeight));
      if (it != args->end()) {
        height = std::get<int>(it->second);
      }

      auto x = 0;
      it = args->find(flutter::EncodableValue(kArgX));
      if (it != args->end()) {
        x = std::get<int32_t>(it->second);
      }

      auto y = 0;
      it = args->find(flutter::EncodableValue(kArgY));
      if (it != args->end()) {
        y = std::get<int32_t>(it->second);
      }

      auto index =
          view->CreateSurface(h_module, assets_path, cache_folder, type,
                              z_order, sync, width, height, x, y);

      auto context = view->GetSurfaceContext(index);

      auto value = flutter::EncodableValue(flutter::EncodableMap{
          {flutter::EncodableValue("result"), flutter::EncodableValue(0)},
          {flutter::EncodableValue("context"),
           flutter::EncodableValue(reinterpret_cast<int64_t>(context))},
          {flutter::EncodableValue("index"),
           flutter::EncodableValue(static_cast<int>(index))},
      });

      result = codec.EncodeSuccessEnvelope(&value);

    } else {
      result = codec.EncodeErrorEnvelope("argument_error", "Invalid Arguments");
    }
  } else if (method == kMethodDispose) {
    if (!obj->arguments()->IsNull()) {
      auto args = std::get_if<flutter::EncodableMap>(obj->arguments());

      int64_t viewId = 0;
      auto it = args->find(flutter::EncodableValue(kArgView));
      if (it != args->end()) {
        flutter::EncodableValue encodedValue = it->second;
        viewId = encodedValue.LongValue();
      }

      auto view = engine->GetView();
      if (viewId != view->GetIndex()) {
        assert(false);
      }

      auto index = 0;
      it = args->find(flutter::EncodableValue(kSurfaceIndex));
      if (it != args->end()) {
        index = std::get<int>(it->second);
      }

      view->DisposeSurface(index);

      result = codec.EncodeSuccessEnvelope();
    } else {
      result = codec.EncodeErrorEnvelope("argument_error", "Invalid Arguments");
    }
  }
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
