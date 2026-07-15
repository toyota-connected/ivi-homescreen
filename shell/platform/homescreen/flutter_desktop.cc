// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter_desktop_plugin_registrar.h"
#include "flutter_desktop_texture_registrar.h"
#include "logging/logging.h"

#include "flutter_homescreen.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>

#include <asio/post.hpp>

#include "flutter_desktop_engine_state.h"
#include "flutter_desktop_messenger.h"
#include "flutter_desktop_view.h"
#include "flutter_desktop_view_controller_state.h"

#include "backend/backend.h"
#include "text_input_plugin.h"
#include "view/flutter_view.h"

#include "libflutter_engine.h"

#if !defined(GL_RGBA8)
#define GL_RGBA8 0x8058
#endif

struct FlutterDesktopEngineState;
struct FlutterDesktopView;

static_assert(FLUTTER_ENGINE_VERSION == 1, "Engine version does not match");

// Attempts to load AOT data from the given path, which must be absolute and
// non-empty. Logs and returns nullptr on failure.
std::unique_ptr<_FlutterEngineAOTData, AOTDataDeleter> LoadAotData(
    const std::filesystem::path& aot_data_path) {
  if (aot_data_path.empty()) {
    ihs::log::error(
        "Attempted to load AOT data, but no aot_data_path was provided.");
    return nullptr;
  }
  const std::string path_string = aot_data_path.string();
  if (!std::filesystem::exists(aot_data_path)) {
    ihs::log::error("Can't load AOT data from {}; no such file.", path_string);
    return nullptr;
  }
  FlutterEngineAOTDataSource source = {};
  source.type = kFlutterEngineAOTDataSourceTypeElfPath;
  source.elf_path = path_string.c_str();
  FlutterEngineAOTData data = nullptr;
  if (LibFlutterEngine->CreateAOTData(&source, &data) != kSuccess) {
    ihs::log::error("Failed to load AOT data from: {}", path_string);
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

#if ENABLE_PLUGINS
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

#if BUILD_WATCHDOG
  // Watchdog plugin.
  state->watchdog_handler = std::make_unique<WatchdogPlugin>(
      state->internal_plugin_registrar->messenger());
#endif
}

FlutterDesktopEngineRef FlutterDesktopGetEngine(
    FlutterDesktopWindowControllerRef controller) {
  return controller->engine_state;
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
  const auto flutter_engine = messenger->GetEngine()->flutter_engine;
  post(*messenger->GetEngine()->platform_task_runner->GetStrandContext(),
       [flutter_engine, promise, channel, message, message_size, reply,
        user_data]() {
         FlutterPlatformMessageResponseHandle* response_handle = nullptr;
         if (reply != nullptr && user_data != nullptr) {
           const FlutterEngineResult result =
               LibFlutterEngine->PlatformMessageCreateResponseHandle(
                   flutter_engine, reply, user_data, &response_handle);
           if (result != kSuccess) {
             ihs::log::error("Failed to create response handle");
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
             LibFlutterEngine->SendPlatformMessage(flutter_engine,
                                                   platform_message.release());

         if (response_handle != nullptr) {
           LibFlutterEngine->PlatformMessageReleaseResponseHandle(
               flutter_engine, response_handle);
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
  // The singleton plugin_common_glib::MainLoop outlives the engine; its
  // glib thread can dispatch a pending gst bus message to OnBusMessage
  // on a freed VideoPlayer after teardown, which lands here via
  // EventSink::Success → BinaryMessenger::Send. ~FlutterView calls
  // SetEngine(nullptr) on the messenger before m_state destructs; we
  // bail on null engine here so that late callbacks don't deref freed
  // state. The lock is held only for the read — releasing it before
  // f.wait() below, otherwise SetEngine(nullptr) and the wait deadlock
  // each other (wait can't complete during shutdown).
  FlutterDesktopEngineState* engine = nullptr;
  {
    std::scoped_lock lock(messenger->GetMutex());
    engine = messenger->GetEngine();
  }
  if (engine == nullptr || engine->platform_task_runner == nullptr) {
    return false;
  }
  if (const auto task_runner = engine->platform_task_runner;
      task_runner->IsThreadEqual(pthread_self())) {
    FlutterPlatformMessageResponseHandle* response_handle = nullptr;
    if (reply != nullptr && user_data != nullptr) {
      const FlutterEngineResult result =
          LibFlutterEngine->PlatformMessageCreateResponseHandle(
              messenger->GetEngine()->flutter_engine, reply, user_data,
              &response_handle);
      if (result != kSuccess) {
        ihs::log::error("Failed to create response handle");
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

  auto f = PostMessengerSendWithReply(messenger, channel, message, message_size,
                                      reply, user_data);
  f.wait();
  return f.get();
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

bool FlutterDesktopPluginRegistrarGetEglContext(
    FlutterDesktopPluginRegistrarRef registrar,
    FlutterDesktopEglContext* out) {
  if (!out) {
    return false;
  }
  out->display = nullptr;
  out->config = nullptr;
  out->share_context = nullptr;
  if (!registrar || !registrar->engine || !registrar->engine->view_controller ||
      !registrar->engine->view_controller->view) {
    return false;
  }
  auto* backend = registrar->engine->view_controller->view->GetBackend();
  if (!backend) {
    return false;
  }
  BackendEglContext ctx{};
  if (!backend->GetEglContext(&ctx)) {
    return false;
  }
  out->display = ctx.display;
  out->config = ctx.config;
  out->share_context = ctx.share_context;
  return true;
}

int64_t FlutterDesktopTextureRegistrarRegisterExternalTexture(
    FlutterDesktopTextureRegistrarRef texture_registrar,
    const FlutterDesktopTextureInfo* texture_info) {
  std::scoped_lock lock(texture_registrar->texture_mutex);
  int64_t result = -1;

  if (texture_info->type == kFlutterDesktopPixelBufferTexture) {
    const auto& pb_config = texture_info->pixel_buffer_config;
    if (!pb_config.callback) {
      ihs::log::error(
          "RegisterExternalTexture: pixel_buffer_config.callback is null");
      return result;
    }

    // Pixel-buffer texture ids live in a disjoint range from GPU-surface
    // ids (which reuse the plugin-owned GLuint name, at most 32 bits), so
    // they can never collide in the registry.
    static std::atomic<int64_t> next_pixel_buffer_id{1LL << 48};
    const int64_t id = next_pixel_buffer_id.fetch_add(1);

    auto desc = std::make_unique<GL_TEXTURE_2D_DESC>();
    desc->target = 0;
    desc->name = 0;
    desc->format = 0;
    desc->width = 0;
    desc->height = 0;
    desc->visible_width = 0;
    desc->visible_height = 0;
    desc->release_callback = nullptr;
    desc->release_context = nullptr;
    desc->pixel_buffer_callback = pb_config.callback;
    desc->pixel_buffer_user_data = pb_config.user_data;

    texture_registrar->texture_registry[id] = std::move(desc);

    IHS_TRACE("RegisterExternalTexture (pixel-buffer): {}, {}",
              fmt::ptr(texture_registrar->engine->flutter_engine), id);
    if (kSuccess == LibFlutterEngine->RegisterExternalTexture(
                        texture_registrar->engine->flutter_engine, id)) {
      result = id;
    } else {
      texture_registrar->texture_registry.erase(id);
    }
  } else if (texture_info->type == kFlutterDesktopGpuSurfaceTexture) {
    const auto& gpu_surface_config = texture_info->gpu_surface_config;
    if (gpu_surface_config.type != kFlutterDesktopGpuSurfaceTypeGlTexture2D) {
      ihs::log::error(
          "RegisterExternalTexture: kFlutterDesktopGpuSurfaceTypeGlTexture2D "
          "is only supported at this time");
      return result;
    }

    // get the client defined descriptor
    const auto descriptor =
        gpu_surface_config.callback(0, 0, gpu_surface_config.user_data);

    if (!descriptor->handle) {
      ihs::log::critical(
          "Descriptor handle is not set.  Assign the address of the texture_id "
          "variable.");
      return result;
    }
    if (descriptor->struct_size != sizeof(FlutterDesktopGpuSurfaceDescriptor)) {
      ihs::log::critical(
          "Descriptor struct_size is not valid.  Set struct_size to "
          "sizeof(FlutterDesktopGpuSurfaceTextureConfig)"
          "is another problem.");
      return result;
    }

    GLuint id = *static_cast<GLuint*>(descriptor->handle);

    // check for existing entry
    for (auto& [fst, snd] : texture_registrar->texture_registry) {
      if (fst == id) {
        snd.reset();
        texture_registrar->texture_registry.erase(id);
        break;
      }
    }

    texture_registrar->texture_registry[id] =
        std::make_unique<GL_TEXTURE_2D_DESC>();
    const auto& val = texture_registrar->texture_registry[id];
    val->name = static_cast<uint32_t>(id);
    val->width = static_cast<uint32_t>(descriptor->width);
    val->height = static_cast<uint32_t>(descriptor->height);
    val->release_callback = descriptor->release_callback;
    val->release_context = descriptor->release_context;
    val->target = GL_TEXTURE_2D;
    val->format = GL_RGBA8;

    IHS_TRACE("RegisterExternalTexture: {}, {}",
              fmt::ptr(texture_registrar->engine->flutter_engine), id);
    if (kSuccess == LibFlutterEngine->RegisterExternalTexture(
                        texture_registrar->engine->flutter_engine, id)) {
      result = id;
    }
  }
  if (result < 0) {
    ihs::log::error("Failed to Register Texture");
  }
  return result;
}

void FlutterDesktopTextureRegistrarUnregisterExternalTexture(
    FlutterDesktopTextureRegistrarRef texture_registrar,
    const int64_t texture_id,
    void (*callback)(void* user_data),
    void* user_data) {
  std::unique_ptr<GL_TEXTURE_2D_DESC> removed;
  {
    std::scoped_lock<std::mutex> lock(texture_registrar->texture_mutex);
    LibFlutterEngine->UnregisterExternalTexture(
        texture_registrar->engine->flutter_engine, texture_id);
    if (const auto it = texture_registrar->texture_registry.find(texture_id);
        it != texture_registrar->texture_registry.end()) {
      removed = std::move(it->second);
      texture_registrar->texture_registry.erase(it);
    }
  }

  if (removed) {
    // Pixel-buffer textures: the embedder owns the GL texture, so free it
    // under a currently-made texture context. Guard the full backend chain
    // in case the engine is mid-teardown.
    //
    // GLESv2 is only linked into the final binary when a GL-based backend
    // is enabled (Wayland EGL, DRM KMS EGL). Wayland Vulkan
    // builds don't link it and don't produce GL pixel-buffer textures at
    // runtime — but the linker still needs the glDeleteTextures symbol
    // unless we compile this branch out.
    if (removed->pixel_buffer_callback && removed->name != 0) {
#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_DRM_KMS_EGL
      Backend* backend = nullptr;
      if (texture_registrar->engine &&
          texture_registrar->engine->view_controller &&
          texture_registrar->engine->view_controller->view) {
        backend =
            texture_registrar->engine->view_controller->view->GetBackend();
      }
      if (backend && backend->TextureMakeCurrent()) {
        GLuint name = removed->name;
        glDeleteTextures(1, &name);
        backend->TextureClearCurrent();
      } else {
        ihs::log::warn(
            "UnregisterExternalTexture: backend texture context unavailable; "
            "GL texture {} leaked",
            removed->name);
      }
#else
      // No GL backend in this configuration; the texture was never
      // actually backed by a live GLuint. Nothing to free.
      (void)texture_registrar;
#endif
    }
    if (removed->release_callback != nullptr) {
      removed->release_callback(removed->release_context);
    }
  }
  if (callback != nullptr) {
    callback(user_data);
  }
}

bool FlutterDesktopTextureRegistrarMarkExternalTextureFrameAvailable(
    FlutterDesktopTextureRegistrarRef texture_registrar,
    int64_t texture_id) {
  if (texture_registrar->shutting_down.load(std::memory_order_acquire)) {
    return false;
  }
  IHS_TRACE("MarkExternalTextureFrameAvailable: {}, {}",
            fmt::ptr(texture_registrar->engine->flutter_engine), texture_id);
  const auto result = LibFlutterEngine->MarkExternalTextureFrameAvailable(
      texture_registrar->engine->flutter_engine, texture_id);
  return result == kSuccess;
}

bool FlutterDesktopTextureMakeCurrent(
    FlutterDesktopTextureRegistrarRef texture_registrar) {
  if (texture_registrar->shutting_down.load(std::memory_order_acquire)) {
    return false;
  }
  const auto backend =
      texture_registrar->engine->view_controller->view->GetBackend();
  IHS_TRACE("TextureMakeCurrent: {}", fmt::ptr(backend));
  return backend->TextureMakeCurrent();
}

bool FlutterDesktopTextureClearCurrent(
    FlutterDesktopTextureRegistrarRef texture_registrar) {
  if (texture_registrar->shutting_down.load(std::memory_order_acquire)) {
    return false;
  }
  const auto backend =
      texture_registrar->engine->view_controller->view->GetBackend();
  IHS_TRACE("TextureClearCurrent: {}", fmt::ptr(backend));
  return backend->TextureClearCurrent();
}

// Passes character input events to registered handlers.
void CharCallback(FlutterDesktopViewControllerState* view_state,
                  const unsigned int code_point) {
  ihs::log::info("CharCallback: {}", code_point);
  for (const auto& handler : view_state->keyboard_hook_handlers) {
    handler->CharHook(code_point);
  }
}

// Passes raw key events to registered handlers.
void KeyCallback(FlutterDesktopViewControllerState* view_state,
                 bool released,
                 xkb_keysym_t keysym,
                 uint32_t xkb_scancode,
                 const uint32_t modifiers) {
  ihs::log::debug("KeyCallback: released: {}, keysym: {}, xkb_scancode: {}",
                  released, keysym, xkb_scancode);
  for (const auto& handler : view_state->keyboard_hook_handlers) {
    handler->KeyboardHook(released, keysym, xkb_scancode, modifiers);
  }
}

// Notifies handlers that keyboard focus has been lost so they can clear any
// pressed-key tracking state.
void FocusLostCallback(FlutterDesktopViewControllerState* view_state) {
  for (const auto& handler : view_state->keyboard_hook_handlers) {
    handler->FocusLost();
  }
}

// Notifies handlers that the active XKB keymap has changed so they can
// recompute modifier bitmasks.
void KeymapChangedCallback(FlutterDesktopViewControllerState* view_state,
                           xkb_keymap* keymap) {
  for (const auto& handler : view_state->keyboard_hook_handlers) {
    handler->KeymapChanged(keymap);
  }
  // Also notify the TextInputPlugin (not in keyboard_hook_handlers).
  if (view_state->text_input_plugin) {
    view_state->text_input_plugin->KeymapChanged(keymap);
  }
}
