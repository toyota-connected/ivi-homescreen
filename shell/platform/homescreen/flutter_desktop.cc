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

#include "view/flutter_view.h"

#include "libflutter_engine.h"

#if !defined(GL_RGBA8)
#define GL_RGBA8 0x8058
#endif

struct FlutterDesktopEngineState;
struct FlutterDesktopView;

static_assert(FLUTTER_ENGINE_VERSION == 1, "Engine version does not match");

std::mutex texture_mutex;

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

#if !DISABLE_PLUGINS
  // Platform Views handler.
  state->platform_views_handler = std::make_unique<PlatformViewsHandler>(
      state->internal_plugin_registrar->messenger(), state);
#endif

  // Mouse Cursor handler.
  state->mouse_cursor_handler = std::make_unique<MouseCursorHandler>(
      state->internal_plugin_registrar->messenger(), view);

  // Logging handler.
  state->logging_handler = std::make_unique<LoggingHandler>(
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

const char* FlutterDesktopPluginRegistrarGetFlutterAssetFolder(
    FlutterDesktopPluginRegistrarRef registrar) {
  return registrar->engine->flutter_asset_directory.c_str();
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

               auto platform_message = std::make_unique<FlutterPlatformMessage>();
               platform_message->struct_size = sizeof(FlutterPlatformMessage);
               platform_message->channel = channel;
               platform_message->message = message;
               platform_message->message_size = message_size;
               platform_message->response_handle = response_handle;

               const FlutterEngineResult message_result =
                   LibFlutterEngine->SendPlatformMessage(
                       messenger->GetEngine()->flutter_engine,
                       platform_message.release());

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
    FlutterDesktopTextureRegistrarRef texture_registrar,
    const FlutterDesktopTextureInfo* texture_info) {
  std::scoped_lock<std::mutex> lock(texture_mutex);
  int64_t result = -1;

  if (texture_info->type == kFlutterDesktopPixelBufferTexture) {
    spdlog::error("RegisterExternalTexture: Pixel Buffer not implemented.");

  } else if (texture_info->type == kFlutterDesktopGpuSurfaceTexture) {
    auto gpu_surface_texture = texture_info->gpu_surface_config;
    if (gpu_surface_texture.type != kFlutterDesktopGpuSurfaceTypeGlTexture2D) {
      spdlog::error(
          "RegisterExternalTexture: kFlutterDesktopGpuSurfaceTypeGlTexture2D "
          "is only supported at this time");
      return result;
    }

    // get the client defined descriptor
    auto descriptor = gpu_surface_texture.callback(
        0, 0, texture_info->gpu_surface_config.user_data);

    if (!descriptor->handle) {
      spdlog::critical(
          "Descriptor handle is not set.  Assign the address of the texture_id "
          "variable.");
      return result;
    } else if (descriptor->struct_size !=
               sizeof(FlutterDesktopGpuSurfaceDescriptor)) {
      spdlog::critical(
          "Descriptor struct_size is not valid.  Set struct_size to "
          "sizeof(FlutterDesktopGpuSurfaceTextureConfig)"
          "is another problem.");
      return result;
    }

    GLuint id = *static_cast<GLuint*>(descriptor->handle);

    // check for existing entry
    for (auto& it : texture_registrar->texture_registry) {
      if (it.first == id) {
        it.second.reset();
        texture_registrar->texture_registry.erase(id);
        break;
      }
    }

    texture_registrar->texture_registry[id] =
        std::make_unique<GL_TEXTURE_2D_DESC>();
    auto& val = texture_registrar->texture_registry[id];
    val->name = static_cast<uint32_t>(id);
    val->width = static_cast<uint32_t>(descriptor->width);
    val->height = static_cast<uint32_t>(descriptor->height);
    val->release_callback = descriptor->release_callback;
    val->release_context = descriptor->release_context;
    val->target = GL_TEXTURE_2D;
    val->format = GL_RGBA8;

    SPDLOG_TRACE("RegisterExternalTexture: {}, {}",
                 fmt::ptr(texture_registrar->engine->flutter_engine), id);
    if (kSuccess == LibFlutterEngine->RegisterExternalTexture(
                        texture_registrar->engine->flutter_engine, id)) {
      result = id;
    }
  }
  if (result < 0) {
    spdlog::error("Failed to Register Texture");
  }
  return result;
}

void FlutterDesktopTextureRegistrarUnregisterExternalTexture(
    FlutterDesktopTextureRegistrarRef texture_registrar,
    int64_t texture_id,
    void (*callback)(void* user_data),
    void* user_data) {
  std::scoped_lock<std::mutex> lock(texture_mutex);
  LibFlutterEngine->UnregisterExternalTexture(
      texture_registrar->engine->flutter_engine, texture_id);
  auto& val = texture_registrar->texture_registry[texture_id];
  if (val && val->release_callback != nullptr) {
    val->release_callback(val->release_context);
  }
  texture_registrar->texture_registry[texture_id].reset();
  texture_registrar->texture_registry.erase(texture_id);
  if (callback != nullptr) {
    callback(user_data);
  }
}

bool FlutterDesktopTextureRegistrarMarkExternalTextureFrameAvailable(
    FlutterDesktopTextureRegistrarRef texture_registrar,
    int64_t texture_id) {
  SPDLOG_TRACE("MarkExternalTextureFrameAvailable: {}, {}",
               fmt::ptr(texture_registrar->engine->flutter_engine), texture_id);
  auto result = LibFlutterEngine->MarkExternalTextureFrameAvailable(
      texture_registrar->engine->flutter_engine, texture_id);
  return result == kSuccess;
}

bool FlutterDesktopTextureMakeCurrent(
    FlutterDesktopTextureRegistrarRef texture_registrar) {
  auto backend = texture_registrar->engine->view_controller->view->GetBackend();
  SPDLOG_TRACE("TextureMakeCurrent: {}", fmt::ptr(backend));
  return backend->TextureMakeCurrent();
}

bool FlutterDesktopTextureClearCurrent(
    FlutterDesktopTextureRegistrarRef texture_registrar) {
  auto backend = texture_registrar->engine->view_controller->view->GetBackend();
  SPDLOG_TRACE("TextureClearCurrent: {}", fmt::ptr(backend));
  return backend->TextureClearCurrent();
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
  spdlog::debug("KeyCallback: released: {}, keysym: {}, xkb_scancode: {}",
                released, keysym, xkb_scancode);
  for (const auto& handler : view_state->keyboard_hook_handlers) {
    handler->KeyboardHook(released, keysym, xkb_scancode, modifiers);
  }
}
