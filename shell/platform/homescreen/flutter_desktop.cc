// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter_desktop_plugin_registrar.h"
#include "flutter_desktop_texture_registrar.h"

#include "flutter_homescreen.h"

#include <algorithm>
#include <filesystem>
#include <string>

#include <wayland/window.h>
#include "flutter/shell/platform/common/client_wrapper/include/flutter/plugin_registrar.h"
#include "flutter/shell/platform/common/incoming_message_dispatcher.h"
#include "flutter_desktop_engine_state.h"
#include "flutter_desktop_messenger.h"
#include "flutter_desktop_plugin_registrar.h"
#include "flutter_desktop_view.h"
#include "flutter_desktop_view_controller_state.h"
#include "shell/libflutter_engine.h"

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
// View is optional; if present it will be provided to the created
// PlatformHandler.
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

FlutterDesktopWindowRef FlutterDesktopPluginRegistrarGetWindow(
    FlutterDesktopPluginRegistrarRef registrar) {
  FlutterDesktopViewControllerState const* controller =
      registrar->engine->view_controller;
  if (!controller) {
    return nullptr;
  }
  return controller->view_wrapper.get();
}

bool FlutterDesktopMessengerSendWithReply(FlutterDesktopMessengerRef messenger,
                                          const char* channel,
                                          const uint8_t* message,
                                          const size_t message_size,
                                          const FlutterDesktopBinaryReply reply,
                                          void* user_data) {
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

void FlutterDesktopMessengerSetCallback(
    FlutterDesktopMessengerRef messenger,
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
  SPDLOG_ERROR("[FlutterDesktopTextureRegistrarRegisterExternalTexture] Not implemented yet.");
  return -1;
}

void FlutterDesktopTextureRegistrarUnregisterExternalTexture(
    FlutterDesktopTextureRegistrarRef /* texture_registrar */,
    int64_t /* texture_id */,
    void (* /* callback */)(void* user_data),
    void* /* user_data */) {
  SPDLOG_ERROR("[FlutterDesktopTextureRegistrarUnregisterExternalTexture] Not implemented yet.");
}

bool FlutterDesktopTextureRegistrarMarkExternalTextureFrameAvailable(
    FlutterDesktopTextureRegistrarRef /* texture_registrar */,
    int64_t /* texture_id */) {
  SPDLOG_ERROR("[FlutterDesktopTextureRegistrarMarkExternalTextureFrameAvailable] Not implemented yet.");
  return false;
}
