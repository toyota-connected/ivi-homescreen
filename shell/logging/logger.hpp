// shell/logging/logger.hpp
// Mux header for IHS logging. Routes to the DLT bridge (when ENABLE_DLT) or
// leaves spdlog as the backend. The near-term focus is keeping this header
// include-safe everywhere; main.cc integration follows separately.
#pragma once

#if defined(ENABLE_DLT)

#  include "dlt/bridge.hpp"
#  include "dlt/compat.hpp"
#  include "dlt/log_level.hpp"

#  include <atomic>
#  include <chrono>
#  include <condition_variable>
#  include <mutex>
#  include <string_view>
#  include <thread>

// Lightweight RAII wrapper around a bridge ContextHandle. Construct one per
// logging site; the bridge caches by id so duplicates are cheap.
class IhsLogContext {
public:
    IhsLogContext(const char* id, const char* description)
        : handle_(ihs::dlt::DltBridge::instance().acquire_context(
              std::string_view{id},
              std::string_view{description})) {}

    [[nodiscard]] bool                          is_valid() const noexcept { return handle_.is_valid(); }
    [[nodiscard]] const ihs::dlt::ContextHandle& impl()    const noexcept { return handle_; }

private:
    ihs::dlt::ContextHandle handle_;
};

#  define IHS_LOGGING_START(app_id_, desc_) \
      ihs::dlt::DltBridge::instance().start((app_id_), (desc_))
#  define IHS_LOGGING_STOP()  ihs::dlt::DltBridge::instance().stop()
#  define IHS_LOGGING_FLUSH() ihs::dlt::DltBridge::instance().flush()

// Flush watchdog: a belt-and-braces thread that periodically calls
// DltBridge::flush() even if the caller forgets. The worker already drains on
// its own schedule; this exists to guarantee forward progress under stalls
// (e.g. a thread holding a ring without further log calls).
class IhsFlushWatchdog {
public:
    explicit IhsFlushWatchdog(
        std::chrono::milliseconds interval = std::chrono::milliseconds(100))
        : interval_(interval) {
        thread_ = std::thread([this] { run(); });
    }

    ~IhsFlushWatchdog() { stop(); }

    IhsFlushWatchdog(const IhsFlushWatchdog&)            = delete;
    IhsFlushWatchdog& operator=(const IhsFlushWatchdog&) = delete;

    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) return;
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void run() {
        std::unique_lock<std::mutex> lock(mu_);
        while (!stop_) {
            if (cv_.wait_for(lock, interval_, [this] { return stop_; })) {
                break;
            }
            lock.unlock();
            ihs::dlt::DltBridge::instance().flush();
            lock.lock();
        }
    }

    std::chrono::milliseconds interval_;
    std::mutex                mu_;
    std::condition_variable   cv_;
    bool                      stop_ = false;
    std::thread               thread_;
};

#  define IHS_LOG_IMPL_(ctx_, level_, ...)                                   \
      do {                                                                   \
          if ((ctx_).is_valid()) {                                           \
              ihs::dlt::DltBridge::instance().logf(                          \
                  (ctx_).impl(), (level_), __VA_ARGS__);                     \
          }                                                                  \
      } while (0)

#  define IHS_LOG_FATAL(ctx_, ...) IHS_LOG_IMPL_((ctx_), ihs::dlt::LogLevel::Fatal,   __VA_ARGS__)
#  define IHS_LOG_ERROR(ctx_, ...) IHS_LOG_IMPL_((ctx_), ihs::dlt::LogLevel::Error,   __VA_ARGS__)
#  define IHS_LOG_WARN(ctx_, ...)  IHS_LOG_IMPL_((ctx_), ihs::dlt::LogLevel::Warn,    __VA_ARGS__)
#  define IHS_LOG_INFO(ctx_, ...)  IHS_LOG_IMPL_((ctx_), ihs::dlt::LogLevel::Info,    __VA_ARGS__)
#  define IHS_LOG_DEBUG(ctx_, ...) IHS_LOG_IMPL_((ctx_), ihs::dlt::LogLevel::Debug,   __VA_ARGS__)
#  define IHS_LOG_TRACE(ctx_, ...) IHS_LOG_IMPL_((ctx_), ihs::dlt::LogLevel::Verbose, __VA_ARGS__)

#else // !ENABLE_DLT

// spdlog backend placeholder — left unwired for now. The macros expand
// to nothing so existing sources can opt into the mux header incrementally.
class IhsLogContext {
public:
    IhsLogContext(const char* /*id*/, const char* /*description*/) {}
    [[nodiscard]] bool is_valid() const noexcept { return false; }
};

#  define IHS_LOGGING_START(app_id_, desc_) ((void)0)
#  define IHS_LOGGING_STOP()                ((void)0)
#  define IHS_LOGGING_FLUSH()               ((void)0)

#  define IHS_LOG_FATAL(ctx_, ...) ((void)0)
#  define IHS_LOG_ERROR(ctx_, ...) ((void)0)
#  define IHS_LOG_WARN(ctx_, ...)  ((void)0)
#  define IHS_LOG_INFO(ctx_, ...)  ((void)0)
#  define IHS_LOG_DEBUG(ctx_, ...) ((void)0)
#  define IHS_LOG_TRACE(ctx_, ...) ((void)0)

#endif // ENABLE_DLT
