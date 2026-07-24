// SPDX-FileCopyrightText: 2026 Toyota Connected North America
// SPDX-License-Identifier: Apache-2.0

// The Flutter desktop C API, re-exported from libihs_shared.so.
//
// A plugin built to the standard embedder headers resolves FlutterDesktop*
// from whatever it is loaded against. The implementation lives in the shell
// (it operates on the engine state), which for an executable-hosted embedder
// is not a shared object a dlopen'd plugin can bind to. So ihs_shared exports
// the functions as forwarders over a proc table the shell installs, the same
// way the trace surface forwards to the engine (see ihs_trace_set_engine_procs)
// -- a plugin then resolves the whole host ABI, ihs_* and Flutter alike, from
// this one library, regardless of what process loaded it.
//
// The forwarders return a safe default until the shell installs the table,
// which it does at engine bring-up, before any plugin's registrar runs.

#ifndef IHS_FLUTTER_DESKTOP_BRIDGE_H_
#define IHS_FLUTTER_DESKTOP_BRIDGE_H_

#include <stddef.h>
#include <stdint.h>

#include <flutter_messenger.h>
#include <flutter_plugin_registrar.h>
#include <flutter_texture_registrar.h>

#include "ihs/ihs_export.h"

#ifdef __cplusplus
extern "C" {
#endif

// The shell's real FlutterDesktop* implementations, as function pointers.
// Filled by the shell (where the names bind unambiguously to those
// implementations) and installed with ihs_flutter_desktop_set_procs. Field
// order and struct_size gate forward-compatible growth.
typedef struct IhsFlutterDesktopProcs {
  size_t struct_size;

  bool (*messenger_send)(FlutterDesktopMessengerRef,
                         const char*,
                         const uint8_t*,
                         size_t);
  bool (*messenger_send_with_reply)(FlutterDesktopMessengerRef,
                                    const char*,
                                    const uint8_t*,
                                    size_t,
                                    FlutterDesktopBinaryReply,
                                    void*);
  void (*messenger_send_response)(FlutterDesktopMessengerRef,
                                  const FlutterDesktopMessageResponseHandle*,
                                  const uint8_t*,
                                  size_t);
  void (*messenger_set_callback)(FlutterDesktopMessengerRef,
                                 const char*,
                                 FlutterDesktopMessageCallback,
                                 void*);
  FlutterDesktopMessengerRef (*messenger_add_ref)(FlutterDesktopMessengerRef);
  void (*messenger_release)(FlutterDesktopMessengerRef);
  bool (*messenger_is_available)(FlutterDesktopMessengerRef);
  FlutterDesktopMessengerRef (*messenger_lock)(FlutterDesktopMessengerRef);
  void (*messenger_unlock)(FlutterDesktopMessengerRef);

  FlutterDesktopMessengerRef (*registrar_get_messenger)(
      FlutterDesktopPluginRegistrarRef);
  const char* (*registrar_get_asset_folder)(FlutterDesktopPluginRegistrarRef);
  FlutterDesktopTextureRegistrarRef (*registrar_get_texture_registrar)(
      FlutterDesktopPluginRegistrarRef);
  void (*registrar_set_destruction_handler)(
      FlutterDesktopPluginRegistrarRef,
      FlutterDesktopOnPluginRegistrarDestroyed);

  int64_t (*texture_register)(FlutterDesktopTextureRegistrarRef,
                              const FlutterDesktopTextureInfo*);
  void (*texture_unregister)(FlutterDesktopTextureRegistrarRef,
                             int64_t,
                             void (*)(void*),
                             void*);
  bool (*texture_mark_frame_available)(FlutterDesktopTextureRegistrarRef,
                                       int64_t);
  bool (*texture_make_current)(FlutterDesktopTextureRegistrarRef);
  bool (*texture_clear_current)(FlutterDesktopTextureRegistrarRef);
} IhsFlutterDesktopProcs;

// Install (or clear, with NULL) the shell's implementations. Shell-only, called
// once at engine bring-up. The pointer must remain valid for the process
// lifetime.
IHS_EXPORT void ihs_flutter_desktop_set_procs(
    const IhsFlutterDesktopProcs* procs);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // IHS_FLUTTER_DESKTOP_BRIDGE_H_
