// shell/logging/dlt/bridge.cpp
#include "bridge.hpp"

#include "ring_slot.hpp"
#include "thread_ring.hpp"

#if ENABLE_DLT
#include "libdlt_loader.hpp"
#endif

namespace ihs::dlt {

DltBridge& DltBridge::instance() {
  static DltBridge the_bridge;
  return the_bridge;
}

DltBridge::DltBridge()
    : registry_(RingRegistry::instance()), worker_(registry_, cache_) {}

DltBridge::~DltBridge() {
  stop();
}

bool DltBridge::start(const char* app_id, const char* description) {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true)) {
    return false;
  }
#if ENABLE_DLT
  LibDltLoader::instance().register_app(app_id, description);
#else
  (void)app_id;
  (void)description;
#endif
  // Resolve sinks from the environment (IHS_LOG_SINK/LEVEL/FILE). There is
  // always at least the console fallback, and console/file sinks work without
  // libdlt, so the drain worker runs regardless of DLT availability.
  sink_set_ = build_sink_set_from_env();
  level_floor_.store(static_cast<std::uint8_t>(sink_set_.level_floor),
                     std::memory_order_relaxed);
  worker_.start(sink_set_.sinks);
  return true;
}

void DltBridge::stop() {
  bool expected = true;
  if (!started_.compare_exchange_strong(expected, false)) {
    return;
  }
  worker_.stop();
#if ENABLE_DLT
  LibDltLoader::instance().unregister_app();
#endif
}

void DltBridge::flush() noexcept {
  if (started_.load(std::memory_order_acquire)) {
    worker_.flush();
  }
}

ContextHandle DltBridge::acquire_context(std::string_view ctx_id,
                                         std::string_view description) {
  // Contexts are valid once logging is started: console/file sinks serve them
  // by tag even with no libdlt. Before start() there is no worker/sink, so hand
  // back an invalid handle and the ring/worker path is skipped entirely.
  if (!started_.load(std::memory_order_acquire)) {
    return ContextHandle{};
  }
  auto result = cache_.ensure(ctx_id, description);
  if (!result.has_value()) {
    return ContextHandle{};
  }
  return ContextHandle{result.value(), true};
}

bool DltBridge::log(const ContextHandle& ctx,
                    LogLevel level,
                    std::string_view message) noexcept {
  if (!ctx.is_valid()) {
    return false;
  }
  // Global severity floor (IHS_LOG_LEVEL): drop records more verbose than the
  // floor before they enter a ring. Levels are Fatal(1)..Verbose(6), so a
  // numerically larger value is more verbose.
  if (static_cast<std::uint8_t>(level) >
      level_floor_.load(std::memory_order_relaxed)) {
    return false;
  }
  ThreadRing& ring = RingRegistry::thread_local_ring();
  return ring.push(ctx.index(), static_cast<std::uint8_t>(level),
                   message.data(), message.size());
}

}  // namespace ihs::dlt
