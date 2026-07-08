// shell/logging/dlt/bridge.cpp
#include "bridge.hpp"

#include "ring_slot.hpp"
#include "thread_ring.hpp"

namespace ihs::dlt {

DltBridge& DltBridge::instance() {
  static DltBridge the_bridge;
  return the_bridge;
}

DltBridge::DltBridge()
    : loader_(LibDltLoader::instance()),
      registry_(RingRegistry::instance()),
      cache_(loader_),
      worker_(registry_, cache_, loader_) {}

DltBridge::~DltBridge() {
  stop();
}

bool DltBridge::start(const char* app_id, const char* description) {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true)) {
    return false;
  }
  loader_.register_app(app_id, description);
  worker_.start();
  return true;
}

void DltBridge::stop() {
  bool expected = true;
  if (!started_.compare_exchange_strong(expected, false)) {
    return;
  }
  worker_.stop();
  loader_.unregister_app();
}

void DltBridge::flush() noexcept {
  if (started_.load(std::memory_order_acquire)) {
    worker_.flush();
  }
}

ContextHandle DltBridge::acquire_context(std::string_view ctx_id,
                                         std::string_view description) {
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
  ThreadRing& ring = registry_.thread_local_ring();
  return ring.push(ctx.index(), static_cast<std::uint8_t>(level),
                   message.data(), message.size());
}

}  // namespace ihs::dlt
