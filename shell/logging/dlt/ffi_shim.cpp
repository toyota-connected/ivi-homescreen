// shell/logging/dlt/ffi_shim.cpp
#include "ffi_shim.hpp"

#include "bridge.hpp"
#include "log_level.hpp"

#include <cstdint>
#include <string_view>

namespace {

// Keep a parallel table of ContextHandles so the FFI boundary can speak in
// plain integer indices. The table is append-only; the bridge's own cache
// already bounds the maximum number of contexts.
struct HandleTable {
  static constexpr std::size_t kMax = 256;
  ihs::dlt::ContextHandle handles[kMax];
  std::size_t count = 0;
};

HandleTable& handle_table() {
  static HandleTable t;
  return t;
}

}  // namespace

extern "C" int ihs_dlt_start(const char* app_id, const char* description) {
  return ihs::dlt::DltBridge::instance().start(app_id, description) ? 1 : 0;
}

extern "C" void ihs_dlt_stop() {
  ihs::dlt::DltBridge::instance().stop();
}

extern "C" void ihs_dlt_flush() {
  ihs::dlt::DltBridge::instance().flush();
}

extern "C" std::int32_t ihs_dlt_acquire_context(const char* ctx_id,
                                                const char* description) {
  if (ctx_id == nullptr) {
    return -1;
  }
  auto handle = ihs::dlt::DltBridge::instance().acquire_context(
      std::string_view{ctx_id}, description != nullptr
                                    ? std::string_view{description}
                                    : std::string_view{});
  if (!handle.is_valid()) {
    return -1;
  }
  HandleTable& t = handle_table();
  if (t.count >= HandleTable::kMax) {
    return -1;
  }
  const auto slot = static_cast<std::int32_t>(t.count);
  t.handles[t.count++] = handle;
  return slot;
}

extern "C" int ihs_dlt_log(std::int32_t ctx_index,
                           std::uint8_t level,
                           const char* text,
                           std::size_t text_len) {
  if (ctx_index < 0 || text == nullptr) {
    return 0;
  }
  HandleTable& t = handle_table();
  if (static_cast<std::size_t>(ctx_index) >= t.count) {
    return 0;
  }
  return ihs::dlt::DltBridge::instance().log(
             t.handles[ctx_index], static_cast<ihs::dlt::LogLevel>(level),
             std::string_view{text, text_len})
             ? 1
             : 0;
}
