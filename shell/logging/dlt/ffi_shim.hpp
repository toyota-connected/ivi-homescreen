// shell/logging/dlt/ffi_shim.hpp
// Minimal C ABI exported by libihs_logging_dlt.so for Dart FFI consumers.
// A wider binding surface is a follow-up.
#pragma once

// Plain C fixed-width / size types: this is a C ABI surface consumed by C and
// Dart ffigen, so the extern "C" declarations must not use std::-qualified
// names (which also aren't guaranteed visible via <cstdint> under libc++).
#include <stddef.h>
#include <stdint.h>

extern "C" {

// Lifecycle
int ihs_dlt_start(const char* app_id, const char* description);
void ihs_dlt_stop();
void ihs_dlt_flush();

// Returns a non-negative context index or -1 on failure.
int32_t ihs_dlt_acquire_context(const char* ctx_id, const char* description);

// Emits a pre-formatted log line. Returns 1 on success, 0 on drop/failure.
int ihs_dlt_log(int32_t ctx_index,
                uint8_t level,
                const char* text,
                size_t text_len);

}  // extern "C"
