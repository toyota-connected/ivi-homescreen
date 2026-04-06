// shell/logging/dlt/bridge.hpp
#pragma once

#include "compat.hpp"
#include "context_cache.hpp"
#include "libdlt_loader.hpp"
#include "log_level.hpp"
#include "ring_registry.hpp"
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

    [[nodiscard]] bool          is_valid() const noexcept { return valid_; }
    [[nodiscard]] std::uint32_t index()    const noexcept { return index_; }

private:
    friend class DltBridge;
    ContextHandle(std::uint32_t idx, bool valid) : index_(idx), valid_(valid) {}

    std::uint32_t index_ = 0;
    bool          valid_ = false;
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

    // Hot path — append a pre-formatted message to the current thread's ring.
    // Wait-free; drops silently on overflow.
    bool log(const ContextHandle& ctx,
             LogLevel             level,
             std::string_view     message) noexcept;

    // Formatted variant. Uses ihs::format_to, so the fmt argument type
    // switches between std::format_string (C++20+) and const char* (C++17).
#if defined(IHS_HAS_FORMAT_TO_N)
    template <class... Args>
    bool logf(const ContextHandle&          ctx,
              LogLevel                      level,
              std::format_string<Args...>   fmt,
              Args&&...                     args) noexcept {
        if (!ctx.is_valid()) return false;
        char buf[kSlotTextCapacity];
        const std::size_t n =
            ihs::format_to(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
        return log(ctx, level, std::string_view{buf, n});
    }
#else
    template <class... Args>
    bool logf(const ContextHandle& ctx,
              LogLevel             level,
              const char*          fmt,
              Args&&...            args) noexcept {
        if (!ctx.is_valid()) return false;
        char buf[kSlotTextCapacity];
        const std::size_t n =
            ihs::format_to(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
        return log(ctx, level, std::string_view{buf, n});
    }
#endif

private:
    DltBridge();
    ~DltBridge();
    DltBridge(const DltBridge&)            = delete;
    DltBridge& operator=(const DltBridge&) = delete;

    LibDltLoader&     loader_;
    RingRegistry&     registry_;
    ContextCache      cache_;
    Worker            worker_;
    std::atomic<bool> started_{false};
};

} // namespace ihs::dlt
