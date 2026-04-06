// shell/logging/dlt/ffi_shim.hpp
// Minimal C ABI exported by libihs_logging_dlt.so for Dart FFI consumers.
// Full binding surface lands in Phase 5.
#pragma once

#include <cstdint>

extern "C" {

// Lifecycle
int  ihs_dlt_start(const char* app_id, const char* description);
void ihs_dlt_stop();
void ihs_dlt_flush();

// Returns a non-negative context index or -1 on failure.
std::int32_t ihs_dlt_acquire_context(const char* ctx_id,
                                     const char* description);

// Emits a pre-formatted log line. Returns 1 on success, 0 on drop/failure.
int ihs_dlt_log(std::int32_t ctx_index,
                std::uint8_t level,
                const char*  text,
                std::size_t  text_len);

} // extern "C"
