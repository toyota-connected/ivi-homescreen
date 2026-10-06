// shell/logging/dlt/bridge.hpp
#pragma once

#include "compat.hpp"
#include "context_cache.hpp"
#include "log_level.hpp"
#include "ring_registry.hpp"
#include "sink_set.hpp"
#include "worker.hpp"

#include <atomic>
#include <cstdint>
#include <string_view>

namespace ihs::dlt {

// Opaque context handle surfaced to callers of the mux header. It owns the
// cache index; the actual ABI DltContext lives inside ContextCache.
class ContextHandle {
 public:
  ContextHandle() = default;

  [[nodiscard]] bool is_valid() const noexcept { return valid_; }
  [[nodiscard]] std::uint32_t index() const noexcept { return index_; }

 private:
  friend class DltBridge;
  ContextHandle(std::uint32_t idx, bool valid) : index_(idx), valid_(valid) {}

  std::uint32_t index_ = 0;
  bool valid_ = false;
};

class DltBridge {
 public:
  static DltBridge& instance();

  // Bring the bridge online: load libdlt.so, register the app id, spin up
  // the worker. Safe to call more than once; only the first succeeds.
  bool start(const char* app_id, const char* description);
  void stop();
  void flush() noexcept;

  [[nodiscard]] bool started() const noexcept {
    return started_.load(std::memory_order_acquire);
  }

  // Acquire or create a context handle for (ctx_id, description).
  ContextHandle acquire_context(std::string_view ctx_id,
                                std::string_view description);

  // Fast-path predicate: true if a record at level under ctx would pass the
  // Off / severity-floor gate (mirrors log()'s accept condition, minus the
  // transient ring-overflow drop). Lets a caller skip formatting a message
  // that would be discarded.
  [[nodiscard]] bool enabled(const ContextHandle& ctx,
                             LogLevel level) const noexcept;

  // Hot path — append a pre-formatted message to the current thread's ring.
  // Wait-free; drops silently on overflow. const: it mutates only thread-local
  // ring state, never the bridge object.
  bool log(const ContextHandle& ctx,
           LogLevel level,
           std::string_view message) const noexcept;

  // Formatted variant. Uses ihs::format_to, so the fmt argument type
  // switches between std::format_string (C++20+) and const char* (C++17).
#if defined(IHS_HAS_FORMAT_TO_N)
  template <class... Args>
  bool logf(const ContextHandle& ctx,
            LogLevel level,
            std::format_string<Args...> fmt,
            Args&&... args) noexcept {
    if (!ctx.is_valid())
      return false;
    char buf[kSlotTextCapacity];
    const std::size_t n =
        ihs::format_to(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
    return log(ctx, level, std::string_view{buf, n});
  }
#else
  template <class... Args>
  bool logf(const ContextHandle& ctx,
            LogLevel level,
            const char* fmt,
            Args&&... args) noexcept {
    if (!ctx.is_valid())
      return false;
    char buf[kSlotTextCapacity];
    const std::size_t n =
        ihs::format_to(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
    return log(ctx, level, std::string_view{buf, n});
  }
#endif

 private:
  DltBridge();
  // Never runs: the bridge lives for the process (see instance()).
  ~DltBridge();
  DltBridge(const DltBridge&) = delete;
  DltBridge& operator=(const DltBridge&) = delete;

  // Runs at exit, in every process that inherited the handler; stops the
  // bridge only in the one that started it.
  static void stop_at_exit();
  // True in the process that called start(), false in a fork of it.
  [[nodiscard]] bool in_owner() const noexcept;

  RingRegistry& registry_;
  ContextCache cache_;
  // The process that started the drain thread; a fork has no such thread.
  std::atomic<int> owner_pid_{0};
  Worker worker_;
  SinkSet sink_set_;  // built from the environment at start()
  // Records at or above this severity (numerically <=) are enqueued; more
  // verbose ones are dropped ring-side. Verbose = pass everything.
  std::atomic<std::uint8_t> level_floor_{
      static_cast<std::uint8_t>(LogLevel::Verbose)};
  std::atomic<bool> started_{false};
};

}  // namespace ihs::dlt
