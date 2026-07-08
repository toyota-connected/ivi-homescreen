// shell/logging/tests/load/dlt_stub.cc
//
// Minimal libdlt.so.2 stand-in for load-testing the DLT bridge's emit path
// without a running dlt-daemon. It exports exactly the symbols the runtime
// loader resolves (see libdlt_loader.cpp), passes the version gate, and counts
// dlt_user_log_write_string() calls instead of transmitting anything.
//
// dlt_log_string (the single-shot fast path) is deliberately NOT exported, so
// emit() takes the start/string/finish route through the guarded TLS scratch —
// the more complex path worth exercising under load.
//
// Point the loader at the built library with IHS_DLT_LIBRARY=/path/libdlt.so.2.
// The harness reads dlt_stub_emit_count() back via dlopen(RTLD_NOLOAD).
#include <atomic>
#include <cstddef>
#include <cstdio>

namespace {
std::atomic<unsigned long long> g_emitted{0};
}

extern "C" {

int dlt_check_library_version(const char* /*major*/, const char* /*minor*/) {
  return 0;  // DLT_RETURN_OK — pass the loader's version gate
}

int dlt_get_version(char* buf, std::size_t size) {
  if (buf != nullptr && size > 0) {
    std::snprintf(buf, size, "stub-libdlt 2.18.0");
  }
  return 0;
}

int dlt_register_app(const char* /*app_id*/, const char* /*desc*/) {
  return 0;
}
int dlt_unregister_app(void) {
  return 0;
}

int dlt_register_context(void* /*ctx*/,
                         const char* /*ctx_id*/,
                         const char* /*desc*/) {
  return 0;
}
int dlt_unregister_context(void* /*ctx*/) {
  return 0;
}

// Return > 0 so the bridge's emit() proceeds to write_string/finish.
int dlt_user_log_write_start(void* /*ctx*/, void* /*data*/, int /*level*/) {
  return 1;
}
int dlt_user_log_write_finish(void* /*data*/) {
  return 0;
}

int dlt_user_log_write_string(void* /*data*/, const char* /*text*/) {
  g_emitted.fetch_add(1, std::memory_order_relaxed);
  return 0;
}

// Read back by the harness after the run to confirm end-to-end delivery.
unsigned long long dlt_stub_emit_count(void) {
  return g_emitted.load(std::memory_order_relaxed);
}

}  // extern "C"
