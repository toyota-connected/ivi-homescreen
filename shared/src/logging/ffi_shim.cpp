/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ihs/logging.h"

#include "bridge.hpp"
#include "log_level.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

// Parallel table of ContextHandles so the C boundary can speak in plain
// integer indices. Append-only; the bridge's own cache bounds the maximum
// number of contexts.
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

extern "C" int ihs_log_start(const char* app_id, const char* description) {
  return ihs::dlt::DltBridge::instance().start(app_id, description) ? 1 : 0;
}

extern "C" void ihs_log_stop() {
  ihs::dlt::DltBridge::instance().stop();
}

extern "C" void ihs_log_flush() {
  ihs::dlt::DltBridge::instance().flush();
}

extern "C" int32_t ihs_log_context_open(const char* tag,
                                        const IhsLogContextOptions* options) {
  if (tag == nullptr) {
    return -1;
  }
  // Description comes from the options struct; the DLT context id is derived
  // from the tag (options->dlt_ctx_id is reserved for an explicit override).
  const char* description =
      (options != nullptr && options->description != nullptr)
          ? options->description
          : "";
  auto handle = ihs::dlt::DltBridge::instance().acquire_context(
      std::string_view{tag}, std::string_view{description});
  if (!handle.is_valid()) {
    return -1;
  }
  HandleTable& t = handle_table();
  if (t.count >= HandleTable::kMax) {
    return -1;
  }
  const auto slot = static_cast<int32_t>(t.count);
  t.handles[t.count++] = handle;
  return slot;
}

extern "C" int ihs_log(int32_t ctx_index,
                       uint8_t level,
                       const char* text,
                       size_t text_len) {
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

namespace ihs::dlt {

// The logging capability sub-table, wired into IhsApi::logging by
// ihs_get_api(). Function pointers alias the flat entry points above.
const IhsLoggingApi* logging_api() noexcept {
  static const IhsLoggingApi api = {
      sizeof(IhsLoggingApi), &ihs_log_start,        &ihs_log_stop,
      &ihs_log_flush,        &ihs_log_context_open, &ihs_log,
  };
  return &api;
}

}  // namespace ihs::dlt
