// shell/logging/logger.hpp
// Mux header for IHS logging. Routes to the DLT bridge (when ENABLE_DLT) or
// leaves spdlog as the backend. The near-term focus is keeping this header
// include-safe everywhere; main.cc integration follows separately.
#pragma once

#if ENABLE_DLT

#include "dlt/bridge.hpp"
#include "dlt/compat.hpp"
#include "dlt/log_level.hpp"

#include <string_view>

// Lightweight RAII wrapper around a bridge ContextHandle. Construct one per
// logging site; the bridge caches by id so duplicates are cheap.
class IhsLogContext {
 public:
  IhsLogContext(const char* id, const char* description)
      : handle_(ihs::dlt::DltBridge::instance().acquire_context(
            std::string_view{id},
            std::string_view{description})) {}

  [[nodiscard]] bool is_valid() const noexcept { return handle_.is_valid(); }
  [[nodiscard]] const ihs::dlt::ContextHandle& impl() const noexcept {
    return handle_;
  }

 private:
  ihs::dlt::ContextHandle handle_;
};

#define IHS_LOGGING_START(app_id_, desc_) \
  ihs::dlt::DltBridge::instance().start((app_id_), (desc_))
#define IHS_LOGGING_STOP() ihs::dlt::DltBridge::instance().stop()
#define IHS_LOGGING_FLUSH() ihs::dlt::DltBridge::instance().flush()

#define IHS_LOG_IMPL_(ctx_, level_, ...)                            \
  do {                                                              \
    if ((ctx_).is_valid()) {                                        \
      ihs::dlt::DltBridge::instance().logf((ctx_).impl(), (level_), \
                                           __VA_ARGS__);            \
    }                                                               \
  } while (0)

#define IHS_LOG_FATAL(ctx_, ...) \
  IHS_LOG_IMPL_((ctx_), ihs::dlt::LogLevel::Fatal, __VA_ARGS__)
#define IHS_LOG_ERROR(ctx_, ...) \
  IHS_LOG_IMPL_((ctx_), ihs::dlt::LogLevel::Error, __VA_ARGS__)
#define IHS_LOG_WARN(ctx_, ...) \
  IHS_LOG_IMPL_((ctx_), ihs::dlt::LogLevel::Warn, __VA_ARGS__)
#define IHS_LOG_INFO(ctx_, ...) \
  IHS_LOG_IMPL_((ctx_), ihs::dlt::LogLevel::Info, __VA_ARGS__)
#define IHS_LOG_DEBUG(ctx_, ...) \
  IHS_LOG_IMPL_((ctx_), ihs::dlt::LogLevel::Debug, __VA_ARGS__)
#define IHS_LOG_TRACE(ctx_, ...) \
  IHS_LOG_IMPL_((ctx_), ihs::dlt::LogLevel::Verbose, __VA_ARGS__)

#else  // !ENABLE_DLT (ENABLE_DLT undefined or defined to 0)

// spdlog backend placeholder — left unwired for now. The macros expand
// to nothing so existing sources can opt into the mux header incrementally.
class IhsLogContext {
 public:
  IhsLogContext(const char* /*id*/, const char* /*description*/) {}
  [[nodiscard]] bool is_valid() const noexcept { return false; }
};

#define IHS_LOGGING_START(app_id_, desc_) ((void)0)
#define IHS_LOGGING_STOP() ((void)0)
#define IHS_LOGGING_FLUSH() ((void)0)

#define IHS_LOG_FATAL(ctx_, ...) ((void)0)
#define IHS_LOG_ERROR(ctx_, ...) ((void)0)
#define IHS_LOG_WARN(ctx_, ...) ((void)0)
#define IHS_LOG_INFO(ctx_, ...) ((void)0)
#define IHS_LOG_DEBUG(ctx_, ...) ((void)0)
#define IHS_LOG_TRACE(ctx_, ...) ((void)0)

#endif  // ENABLE_DLT
