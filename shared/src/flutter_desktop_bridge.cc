// SPDX-FileCopyrightText: 2026 Toyota Connected North America
// SPDX-License-Identifier: Apache-2.0

// Forwarders for the Flutter desktop C API (see flutter_desktop_bridge.h).
//
// FLUTTER_DESKTOP_LIBRARY makes the FLUTTER_EXPORT-annotated declarations
// resolve to default visibility here, so the version script can keep them in
// the dynamic symbol table; every other symbol stays local.

#define FLUTTER_DESKTOP_LIBRARY

#include "ihs/flutter_desktop_bridge.h"

#include <atomic>

namespace {

// Installed once by the shell; read on every forwarded call. Acquire/release
// pairs with the store so a plugin thread sees a fully-published table.
std::atomic<const IhsFlutterDesktopProcs*> g_procs{nullptr};

const IhsFlutterDesktopProcs* procs() {
  return g_procs.load(std::memory_order_acquire);
}

}  // namespace

extern "C" void ihs_flutter_desktop_set_procs(
    const IhsFlutterDesktopProcs* procs) {
  // Refuse a table too small to cover every field the forwarders call through.
  // The struct only ever grows by appending, so a table shorter than the
  // current definition means an older shell against a newer ihs_shared: the
  // trailing fields would be absent and calling through them undefined. Leaving
  // the forwarders on their safe defaults is a degraded but well-defined state.
  if (procs != nullptr && procs->struct_size < sizeof(IhsFlutterDesktopProcs)) {
    procs = nullptr;
  }
  g_procs.store(procs, std::memory_order_release);
}

// ---- messenger -------------------------------------------------------------

extern "C" bool FlutterDesktopMessengerSend(
    FlutterDesktopMessengerRef messenger,
    const char* channel,
    const uint8_t* message,
    const size_t message_size) {
  const auto* p = procs();
  return p != nullptr
             ? p->messenger_send(messenger, channel, message, message_size)
             : false;
}

extern "C" bool FlutterDesktopMessengerSendWithReply(
    FlutterDesktopMessengerRef messenger,
    const char* channel,
    const uint8_t* message,
    const size_t message_size,
    const FlutterDesktopBinaryReply reply,
    void* user_data) {
  const auto* p = procs();
  return p != nullptr
             ? p->messenger_send_with_reply(messenger, channel, message,
                                            message_size, reply, user_data)
             : false;
}

extern "C" void FlutterDesktopMessengerSendResponse(
    FlutterDesktopMessengerRef messenger,
    const FlutterDesktopMessageResponseHandle* handle,
    const uint8_t* data,
    size_t data_length) {
  const auto* p = procs();
  if (p != nullptr) {
    p->messenger_send_response(messenger, handle, data, data_length);
  }
}

extern "C" void FlutterDesktopMessengerSetCallback(
    FlutterDesktopMessengerRef messenger,
    const char* channel,
    FlutterDesktopMessageCallback callback,
    void* user_data) {
  const auto* p = procs();
  if (p != nullptr) {
    p->messenger_set_callback(messenger, channel, callback, user_data);
  }
}

extern "C" FlutterDesktopMessengerRef FlutterDesktopMessengerAddRef(
    FlutterDesktopMessengerRef messenger) {
  const auto* p = procs();
  return p != nullptr ? p->messenger_add_ref(messenger) : nullptr;
}

extern "C" void FlutterDesktopMessengerRelease(
    FlutterDesktopMessengerRef messenger) {
  const auto* p = procs();
  if (p != nullptr) {
    p->messenger_release(messenger);
  }
}

extern "C" bool FlutterDesktopMessengerIsAvailable(
    FlutterDesktopMessengerRef messenger) {
  const auto* p = procs();
  return p != nullptr ? p->messenger_is_available(messenger) : false;
}

extern "C" FlutterDesktopMessengerRef FlutterDesktopMessengerLock(
    FlutterDesktopMessengerRef messenger) {
  const auto* p = procs();
  return p != nullptr ? p->messenger_lock(messenger) : nullptr;
}

extern "C" void FlutterDesktopMessengerUnlock(
    FlutterDesktopMessengerRef messenger) {
  const auto* p = procs();
  if (p != nullptr) {
    p->messenger_unlock(messenger);
  }
}

// ---- plugin registrar ------------------------------------------------------

extern "C" FlutterDesktopMessengerRef FlutterDesktopPluginRegistrarGetMessenger(
    FlutterDesktopPluginRegistrarRef registrar) {
  const auto* p = procs();
  return p != nullptr ? p->registrar_get_messenger(registrar) : nullptr;
}

extern "C" const char* FlutterDesktopPluginRegistrarGetFlutterAssetFolder(
    FlutterDesktopPluginRegistrarRef registrar) {
  const auto* p = procs();
  return p != nullptr ? p->registrar_get_asset_folder(registrar) : nullptr;
}

extern "C" FlutterDesktopTextureRegistrarRef
FlutterDesktopRegistrarGetTextureRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  const auto* p = procs();
  return p != nullptr ? p->registrar_get_texture_registrar(registrar) : nullptr;
}

extern "C" void FlutterDesktopPluginRegistrarSetDestructionHandler(
    FlutterDesktopPluginRegistrarRef registrar,
    FlutterDesktopOnPluginRegistrarDestroyed callback) {
  const auto* p = procs();
  if (p != nullptr) {
    p->registrar_set_destruction_handler(registrar, callback);
  }
}

// ---- texture registrar -----------------------------------------------------

extern "C" int64_t FlutterDesktopTextureRegistrarRegisterExternalTexture(
    FlutterDesktopTextureRegistrarRef texture_registrar,
    const FlutterDesktopTextureInfo* info) {
  const auto* p = procs();
  return p != nullptr ? p->texture_register(texture_registrar, info) : -1;
}

extern "C" void FlutterDesktopTextureRegistrarUnregisterExternalTexture(
    FlutterDesktopTextureRegistrarRef texture_registrar,
    int64_t texture_id,
    void (*callback)(void* user_data),
    void* user_data) {
  const auto* p = procs();
  if (p != nullptr) {
    p->texture_unregister(texture_registrar, texture_id, callback, user_data);
  }
}

extern "C" bool FlutterDesktopTextureRegistrarMarkExternalTextureFrameAvailable(
    FlutterDesktopTextureRegistrarRef texture_registrar,
    int64_t texture_id) {
  const auto* p = procs();
  return p != nullptr
             ? p->texture_mark_frame_available(texture_registrar, texture_id)
             : false;
}

extern "C" bool FlutterDesktopTextureMakeCurrent(
    FlutterDesktopTextureRegistrarRef texture_registrar) {
  const auto* p = procs();
  return p != nullptr ? p->texture_make_current(texture_registrar) : false;
}

extern "C" bool FlutterDesktopTextureClearCurrent(
    FlutterDesktopTextureRegistrarRef texture_registrar) {
  const auto* p = procs();
  return p != nullptr ? p->texture_clear_current(texture_registrar) : false;
}
