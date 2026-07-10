// shell/logging/logger.hpp
//
// Header-only IHS logging context + lifecycle for the shell and in-tree
// plugins. A thin inline layer over the ihs_shared C ABI (ihs/logging.h):
// IhsLogContext wraps an acquired context index, and the IHS_LOG_* macros
// format a line into a stack buffer (ihs::format_to) before handing it to
// ihs_log. No C++ bridge internals cross the boundary — the only symbols
// referenced are the exported ihs_log_* entry points, which are always present
// (the logging surface compiles regardless of ENABLE_DLT; DLT is just one
// sink).
#pragma once

#include "ihs/format.h"
#include "ihs/logging.h"

#include <cstddef>

// Lightweight wrapper around an acquired logging context. Construct one per
// logging site; the bridge caches by id so duplicates are cheap.
class IhsLogContext {
 public:
  IhsLogContext(const char* id, const char* description)
      : index_(open(id, description)) {}

  [[nodiscard]] bool is_valid() const noexcept { return index_ >= 0; }
  [[nodiscard]] int32_t index() const noexcept { return index_; }

 private:
  static int32_t open(const char* id, const char* description) {
    const IhsLogContextOptions options{
        sizeof(IhsLogContextOptions), description, {}};
    return ihs_log_context_open(id, &options);
  }

  int32_t index_ = -1;
};

#define IHS_LOGGING_START(app_id_, desc_) ihs_log_start((app_id_), (desc_))
#define IHS_LOGGING_STOP() ihs_log_stop()
#define IHS_LOGGING_FLUSH() ihs_log_flush()

// Format a line into a stack buffer and emit it through the bridge C ABI. The
// format-string style follows ihs::format_to ("{}" under std::format, printf
// placeholders under the C++17 fallback). The message is formatted (and its
// arguments evaluated) only when the context is valid AND the level passes the
// IHS_LOG_LEVEL floor, so a filtered call site pays nothing beyond the gate.
#define IHS_LOG_IMPL_(ctx_, level_, ...)                                      \
  do {                                                                        \
    const IhsLogContext& ihs_ctx_ = (ctx_);                                   \
    if (ihs_ctx_.is_valid() && ihs_log_enabled(ihs_ctx_.index(), (level_))) { \
      char ihs_buf_[IHS_LOG_TEXT_CAPACITY];                                   \
      const std::size_t ihs_len_ =                                            \
          ihs::format_to(ihs_buf_, sizeof(ihs_buf_), __VA_ARGS__);            \
      ihs_log(ihs_ctx_.index(), (level_), ihs_buf_, ihs_len_);                \
    }                                                                         \
  } while (0)

#define IHS_LOG_FATAL(ctx_, ...) \
  IHS_LOG_IMPL_((ctx_), IHS_LEVEL_FATAL, __VA_ARGS__)
#define IHS_LOG_ERROR(ctx_, ...) \
  IHS_LOG_IMPL_((ctx_), IHS_LEVEL_ERROR, __VA_ARGS__)
#define IHS_LOG_WARN(ctx_, ...) \
  IHS_LOG_IMPL_((ctx_), IHS_LEVEL_WARN, __VA_ARGS__)
#define IHS_LOG_INFO(ctx_, ...) \
  IHS_LOG_IMPL_((ctx_), IHS_LEVEL_INFO, __VA_ARGS__)
#define IHS_LOG_DEBUG(ctx_, ...) \
  IHS_LOG_IMPL_((ctx_), IHS_LEVEL_DEBUG, __VA_ARGS__)
#define IHS_LOG_TRACE(ctx_, ...) \
  IHS_LOG_IMPL_((ctx_), IHS_LEVEL_VERBOSE, __VA_ARGS__)
