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

bool DltBridge::enabled(const ContextHandle& ctx,
                        LogLevel level) const noexcept {
  if (!ctx.is_valid()) {
    return false;
  }
  // Off is a floor sentinel ("log nothing"), not a message severity: a record
  // logged at Off never emits, and an IHS_LOG_LEVEL of off therefore silences
  // everything (it would otherwise pass Off records, since 0 > 0 is false).
  if (level == LogLevel::Off) {
    return false;
  }
  // Global severity floor (IHS_LOG_LEVEL): records more verbose than the floor
  // are dropped. Levels are Fatal(1)..Verbose(6), so a numerically larger value
  // is more verbose.
  return static_cast<std::uint8_t>(level) <=
         level_floor_.load(std::memory_order_relaxed);
}

bool DltBridge::log(const ContextHandle& ctx,
                    LogLevel level,
                    std::string_view message) const noexcept {
  // Apply the Off/floor gate before touching a ring; the caller may already
  // have gated via enabled(), but re-checking here keeps log() correct on its
  // own and is a cheap atomic load.
  if (!enabled(ctx, level)) {
    return false;
  }
  ThreadRing& ring = RingRegistry::thread_local_ring();
  const auto lvl = static_cast<std::uint8_t>(level);

  // A message that fits is one slot, which is the common case and stays a
  // single push.
  constexpr std::size_t kFit = kSlotTextCapacity - 1;
  if (message.size() <= kFit) {
    return ring.push(ctx.index(), lvl, message.data(), message.size());
  }

  // Longer than a slot: split it here rather than let the slot clip it, and
  // mark every piece but the last so the drain can put it back together. Doing
  // this below the C ABI means a caller never has to know the slot size --
  // ihs_log() takes a message of any length and nothing is lost.
  //
  // All or nothing. A run whose last piece failed to enqueue would leave the
  // drain holding a partial with no terminator, and the next unrelated record
  // on this ring would be appended to it and printed as though it belonged --
  // one corrupt line, plus a lost one, from a transient full ring. So take the
  // room up front; the consumer only ever frees more, so a count read here
  // cannot shrink under us.
  const std::size_t pieces = (message.size() + kFit - 1) / kFit;
  if (ring.space() < pieces) {
    // Not enough room for the whole message. Enqueue what one slot holds and
    // let push() mark it clipped, which is exactly the behavior this path had
    // before it learned to split.
    return ring.push(ctx.index(), lvl, message.data(), message.size());
  }
  bool ok = true;
  std::string_view rest = message;
  while (!rest.empty()) {
    const std::size_t take = rest.size() < kFit ? rest.size() : kFit;
    const bool last = take == rest.size();
    ok = ring.push(ctx.index(), lvl, rest.data(), take,
                   last ? 0 : kFlagContinues) &&
         ok;
    rest.remove_prefix(take);
  }
  return ok;
}

}  // namespace ihs::dlt
