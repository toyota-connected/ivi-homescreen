// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter_desktop_plugin_registrar.h"
#include "flutter_desktop_texture_registrar.h"

#include "flutter_homescreen.h"

#include <algorithm>
#include <filesystem>
#include <string>

#include <asio/post.hpp>

#include "flutter_desktop_engine_state.h"
#include "flutter_desktop_messenger.h"
#include "flutter_desktop_view.h"
#include "flutter_desktop_view_controller_state.h"
#include "shell/platform/homescreen/platform_handler.h"

#include "view/flutter_view.h"

struct FlutterDesktopEngineState;
struct FlutterDesktopView;

static_assert(FLUTTER_ENGINE_VERSION == 1, "Engine version does not match");

// Attempts to load AOT data from the given path, which must be absolute and
// non-empty. Logs and returns nullptr on failure.
std::unique_ptr<_FlutterEngineAOTData, AOTDataDeleter> LoadAotData(
    const std::filesystem::path& aot_data_path) {
  if (aot_data_path.empty()) {
    spdlog::error(
        "Attempted to load AOT data, but no aot_data_path was provided.");
    return nullptr;
  }
  const std::string path_string = aot_data_path.string();
  if (!std::filesystem::exists(aot_data_path)) {
    spdlog::error("Can't load AOT data from {}; no such file.", path_string);
    return nullptr;
  }
  FlutterEngineAOTDataSource source = {};
  source.type = kFlutterEngineAOTDataSourceTypeElfPath;
  source.elf_path = path_string.c_str();
  FlutterEngineAOTData data = nullptr;
  if (LibFlutterEngine->CreateAOTData(&source, &data) != kSuccess) {
    spdlog::error("Failed to load AOT data from: {}", path_string);
    return nullptr;
  }
  return std::unique_ptr<_FlutterEngineAOTData, AOTDataDeleter>(data);
}

// Populates |state|'s helper object fields that are common to normal and
// headless mode.
//
void SetUpCommonEngineState(FlutterDesktopEngineState* state,
                            FlutterView* view) {
  // Messaging.
  state->messenger = FlutterDesktopMessengerReferenceOwner(
      FlutterDesktopMessengerAddRef(new FlutterDesktopMessenger()),
      &FlutterDesktopMessengerRelease);
  state->messenger->SetEngine(state);
  state->message_dispatcher =
      std::make_unique<flutter::IncomingMessageDispatcher>(
          state->messenger.get());

  // Plugins.
  state->plugin_registrar = std::make_unique<FlutterDesktopPluginRegistrar>();
  state->plugin_registrar->engine = state;
  state->internal_plugin_registrar =
      std::make_unique<flutter::PluginRegistrar>(state->plugin_registrar.get());

  // Textures.
  state->texture_registrar = std::make_unique<FlutterDesktopTextureRegistrar>();
  state->texture_registrar->engine = state;

  // System channel handler.
  state->platform_handler = std::make_unique<PlatformHandler>(
      state->internal_plugin_registrar->messenger(), view);

  // Platform Views handler.
  state->platform_views_handler = std::make_unique<PlatformViewsHandler>(
      state->internal_plugin_registrar->messenger(), state);

  // Mouse Cursor handler.
  state->mouse_cursor_handler = std::make_unique<MouseCursorHandler>(
      state->internal_plugin_registrar->messenger(), view);
}

FlutterDesktopEngineRef FlutterDesktopGetEngine(
    FlutterDesktopWindowControllerRef controller) {
  return controller->engine_state.get();
}

FlutterDesktopPluginRegistrarRef FlutterDesktopGetPluginRegistrar(
    FlutterDesktopEngineRef engine,
    const char* /* plugin_name */) {
  // Currently, one registrar acts as the registrar for all plugins, so the
  // name is ignored. It is part of the API to reduce churn in the future when
  // aligning more closely with the Flutter registrar system.
  return engine->plugin_registrar.get();
}

void FlutterDesktopPluginRegistrarEnableInputBlocking(
    FlutterDesktopPluginRegistrarRef registrar,
    const char* channel) {
  registrar->engine->message_dispatcher->EnableInputBlockingForChannel(channel);
}

FlutterDesktopMessengerRef FlutterDesktopPluginRegistrarGetMessenger(
    FlutterDesktopPluginRegistrarRef registrar) {
  return registrar->engine->messenger.get();
}

void FlutterDesktopPluginRegistrarSetDestructionHandler(
    FlutterDesktopPluginRegistrarRef registrar,
    FlutterDesktopOnPluginRegistrarDestroyed callback) {
  registrar->destruction_handler = callback;
}

FlutterDesktopEngineRef FlutterDesktopPluginRegistrarGetEngine(
    FlutterDesktopPluginRegistrarRef registrar) {
  return registrar->engine;
}

FlutterDesktopWindowRef FlutterDesktopPluginRegistrarGetWindow(
    FlutterDesktopPluginRegistrarRef registrar) {
  FlutterDesktopViewControllerState const* controller =
      registrar->engine->view_controller;
  if (!controller) {
    return nullptr;
  }
  return controller->view_wrapper.get();
}

std::future<bool> PostMessengerSendWithReply(
    FlutterDesktopMessengerRef messenger,
    const char* channel,
    const uint8_t* message,
    const size_t message_size,
    const FlutterDesktopBinaryReply reply,
    void* user_data) {
  const auto promise(std::make_shared<std::promise<bool>>());
  auto promise_future(promise->get_future());
  asio::post(*messenger->GetEngine()->platform_task_runner->GetStrandContext(),
             [&, promise, channel, message, message_size, reply, user_data]() {
               FlutterPlatformMessageResponseHandle* response_handle = nullptr;
               if (reply != nullptr && user_data != nullptr) {
                 const FlutterEngineResult result =
                     LibFlutterEngine->PlatformMessageCreateResponseHandle(
                         messenger->GetEngine()->flutter_engine, reply,
                         user_data, &response_handle);
                 if (result != kSuccess) {
                   spdlog::error("Failed to create response handle");
                   promise->set_value(false);
                   return;
                 }
               }

               const FlutterPlatformMessage platform_message = {
                   sizeof(FlutterPlatformMessage),
                   channel,
                   message,
                   message_size,
                   response_handle,
               };

               const FlutterEngineResult message_result =
                   LibFlutterEngine->SendPlatformMessage(
                       messenger->GetEngine()->flutter_engine,
                       &platform_message);

               if (response_handle != nullptr) {
                 LibFlutterEngine->PlatformMessageReleaseResponseHandle(
                     messenger->GetEngine()->flutter_engine, response_handle);
               }

               promise->set_value(message_result == kSuccess);
             });
  return promise_future;
}

bool FlutterDesktopMessengerSendWithReply(FlutterDesktopMessengerRef messenger,
                                          const char* channel,
                                          const uint8_t* message,
                                          const size_t message_size,
                                          const FlutterDesktopBinaryReply reply,
                                          void* user_data) {
  auto task_runner = messenger->GetEngine()->platform_task_runner;
  if (task_runner->IsThreadEqual(pthread_self())) {
    FlutterPlatformMessageResponseHandle* response_handle = nullptr;
    if (reply != nullptr && user_data != nullptr) {
      const FlutterEngineResult result =
          LibFlutterEngine->PlatformMessageCreateResponseHandle(
              messenger->GetEngine()->flutter_engine, reply, user_data,
              &response_handle);
      if (result != kSuccess) {
        spdlog::error("Failed to create response handle");
        return false;
      }
    }

    const FlutterPlatformMessage platform_message = {
        sizeof(FlutterPlatformMessage),
        channel,
        message,
        message_size,
        response_handle,
    };

    const FlutterEngineResult message_result =
        LibFlutterEngine->SendPlatformMessage(
            messenger->GetEngine()->flutter_engine, &platform_message);

    if (response_handle != nullptr) {
      LibFlutterEngine->PlatformMessageReleaseResponseHandle(
          messenger->GetEngine()->flutter_engine, response_handle);
    }

    return message_result == kSuccess;
  } else {
    auto f = PostMessengerSendWithReply(messenger, channel, message,
                                        message_size, reply, user_data);
    f.wait();
    return f.get();
  }
}

bool FlutterDesktopMessengerSend(FlutterDesktopMessengerRef messenger,
                                 const char* channel,
                                 const uint8_t* message,
                                 const size_t message_size) {
  return FlutterDesktopMessengerSendWithReply(messenger, channel, message,
                                              message_size, nullptr, nullptr);
}

void FlutterDesktopMessengerSendResponse(
    FlutterDesktopMessengerRef messenger,
    const FlutterDesktopMessageResponseHandle* handle,
    const uint8_t* data,
    const size_t data_length) {
  LibFlutterEngine->SendPlatformMessageResponse(
      messenger->GetEngine()->flutter_engine, handle, data, data_length);
}

void FlutterDesktopMessengerSetCallback(FlutterDesktopMessengerRef messenger,
                                        const char* channel,
                                        FlutterDesktopMessageCallback callback,
                                        void* user_data) {
  messenger->GetEngine()->message_dispatcher->SetMessageCallback(
      channel, callback, user_data);
}

FlutterDesktopTextureRegistrarRef FlutterDesktopRegistrarGetTextureRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  return registrar->engine->texture_registrar.get();
}

int64_t FlutterDesktopTextureRegistrarRegisterExternalTexture(
    FlutterDesktopTextureRegistrarRef /* texture_registrar */,
    const FlutterDesktopTextureInfo* /* texture_info */) {
  SPDLOG_ERROR(
      "[FlutterDesktopTextureRegistrarRegisterExternalTexture] Not implemented "
      "yet.");
  return -1;
}

void FlutterDesktopTextureRegistrarUnregisterExternalTexture(
    FlutterDesktopTextureRegistrarRef /* texture_registrar */,
    int64_t /* texture_id */,
    void (* /* callback */)(void* user_data),
    void* /* user_data */) {
  SPDLOG_ERROR(
      "[FlutterDesktopTextureRegistrarUnregisterExternalTexture] Not "
      "implemented yet.");
}

bool FlutterDesktopTextureRegistrarMarkExternalTextureFrameAvailable(
    FlutterDesktopTextureRegistrarRef /* texture_registrar */,
    int64_t /* texture_id */) {
  SPDLOG_ERROR(
      "[FlutterDesktopTextureRegistrarMarkExternalTextureFrameAvailable] Not "
      "implemented yet.");
  return false;
}

// Passes character input events to registered handlers.
void CharCallback(FlutterDesktopViewControllerState* view_state,
                                const unsigned int code_point) {
  spdlog::info("CharCallback: {}", code_point);
  for (const auto& handler : view_state->keyboard_hook_handlers) {
    handler->CharHook(code_point);
  }
}

// Passes raw key events to registered handlers.
void KeyCallback(FlutterDesktopViewControllerState* view_state,
                               bool released,
                               xkb_keysym_t keysym,
                               uint32_t xkb_scancode,
                               uint32_t modifiers) {
  spdlog::debug("KeyCallback: released: {}, keysym: {}, xkb_scancode: {}", released, keysym, xkb_scancode);
  for (const auto& handler : view_state->keyboard_hook_handlers) {
    handler->KeyboardHook(released, keysym, xkb_scancode, modifiers);
  }
}
