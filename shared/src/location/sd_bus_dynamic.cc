/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "sd_bus_dynamic.hpp"

#include <dlfcn.h>

#include <mutex>

namespace ihs::location {

namespace {

// Resolve one symbol into @slot; records failure in @ok.
template <typename Fn>
void Resolve(void* handle, const char* name, Fn& slot, bool& ok) {
  slot = reinterpret_cast<Fn>(dlsym(handle, name));
  if (slot == nullptr) {
    ok = false;
  }
}

}  // namespace

const SdBusApi* SdBusLoad() {
  static SdBusApi api{};
  static bool loaded = false;
  static std::once_flag once;
  std::call_once(once, [] {
    void* h = dlopen("libsystemd.so.0", RTLD_NOW | RTLD_LOCAL);
    if (h == nullptr) {
      return;  // libsystemd absent -> geoclue unavailable
    }
    bool ok = true;
    Resolve(h, "sd_bus_open_system", api.open_system, ok);
    Resolve(h, "sd_bus_open_user", api.open_user, ok);
    Resolve(h, "sd_bus_new", api.bus_new, ok);
    Resolve(h, "sd_bus_set_address", api.set_address, ok);
    Resolve(h, "sd_bus_set_bus_client", api.set_bus_client, ok);
    Resolve(h, "sd_bus_start", api.start, ok);
    Resolve(h, "sd_bus_call_method", api.call_method, ok);
    Resolve(h, "sd_bus_message_read", api.message_read, ok);
    Resolve(h, "sd_bus_message_unref", api.message_unref, ok);
    Resolve(h, "sd_bus_set_property", api.set_property, ok);
    Resolve(h, "sd_bus_match_signal", api.match_signal, ok);
    Resolve(h, "sd_bus_get_property_trivial", api.get_property_trivial, ok);
    Resolve(h, "sd_bus_process", api.process, ok);
    Resolve(h, "sd_bus_wait", api.wait, ok);
    Resolve(h, "sd_bus_flush_close_unref", api.flush_close_unref, ok);
    Resolve(h, "sd_bus_error_free", api.error_free, ok);
    loaded = ok;
    // Intentionally leak @h: the library stays mapped for the process lifetime
    // (like the DLT loader), so the resolved pointers remain valid.
  });
  return loaded ? &api : nullptr;
}

}  // namespace ihs::location
