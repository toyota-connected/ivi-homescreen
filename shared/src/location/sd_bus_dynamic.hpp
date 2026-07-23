/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef IHS_LOC_SD_BUS_DYNAMIC_HPP_
#define IHS_LOC_SD_BUS_DYNAMIC_HPP_

// Runtime loader for the slice of sd-bus (libsystemd) the geoclue provider
// uses, so ihs_shared gains no build-time or link-time dependency on
// libsystemd — matching how the DLT sink dlopens libdlt. The types below are
// the stable public sd-bus ABI (so this compiles with no libsystemd headers
// present); the functions are resolved from libsystemd.so.0 at first use, and
// if that library is absent geoclue simply reports no fix.

#include <cstdint>

extern "C" {

// Opaque sd-bus handles.
struct sd_bus;
struct sd_bus_message;
struct sd_bus_slot;

// Public sd_bus_error layout (sd-bus.h): { const char*; const char*; int; }.
struct sd_bus_error {
  const char* name;
  const char* message;
  int need_free;
};

using sd_bus_message_handler_t = int (*)(sd_bus_message*, void*, sd_bus_error*);

}  // extern "C"

namespace ihs::location {

// The resolved sd-bus entry points. Signatures mirror sd-bus.h; the variadic
// ones (call_method / message_read / set_property) keep the "..." so callers
// pass the D-Bus argument list exactly as with the real API.
struct SdBusApi {
  int (*open_system)(sd_bus**);
  int (*open_user)(sd_bus**);
  int (*bus_new)(sd_bus**);
  int (*set_address)(sd_bus*, const char*);
  int (*set_bus_client)(sd_bus*, int);
  int (*start)(sd_bus*);
  int (*call_method)(sd_bus*,
                     const char*,
                     const char*,
                     const char*,
                     const char*,
                     sd_bus_error*,
                     sd_bus_message**,
                     const char*,
                     ...);
  int (*message_read)(sd_bus_message*, const char*, ...);
  sd_bus_message* (*message_unref)(sd_bus_message*);
  int (*set_property)(sd_bus*,
                      const char*,
                      const char*,
                      const char*,
                      const char*,
                      sd_bus_error*,
                      const char*,
                      ...);
  int (*match_signal)(sd_bus*,
                      sd_bus_slot**,
                      const char*,
                      const char*,
                      const char*,
                      const char*,
                      sd_bus_message_handler_t,
                      void*);
  int (*get_property_trivial)(sd_bus*,
                              const char*,
                              const char*,
                              const char*,
                              const char*,
                              sd_bus_error*,
                              char,
                              void*);
  int (*process)(sd_bus*, sd_bus_message**);
  int (*wait)(sd_bus*, uint64_t);
  sd_bus* (*flush_close_unref)(sd_bus*);
  void (*error_free)(sd_bus_error*);
};

// Resolve libsystemd.so.0 once (thread-safe) and return its sd-bus entry
// points, or nullptr if the library is not present or a symbol is missing.
const SdBusApi* SdBusLoad();

}  // namespace ihs::location

#endif  // IHS_LOC_SD_BUS_DYNAMIC_HPP_
