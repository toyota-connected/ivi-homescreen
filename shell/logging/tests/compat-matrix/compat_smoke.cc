// shell/logging/tests/compat-matrix/compat_smoke.cc
//
// Compat-matrix smoke test. Exercises the pieces that have to keep compiling
// (and running) across the C++17/20/23 toolchains and both DLT settings:
//
//   1. The logger.hpp lifecycle: IHS_LOGGING_START/STOP/FLUSH.
//   2. IHS_LOG_INFO format strings under both the std::format backend ("{}")
//      and the snprintf fallback ("%d"/"%s"), guarded by the same IHS_HAS_*
//      macros that switch the backend inside ihs::format_to.
//   3. The direct DltBridge::log() path (pre-formatted strings).
//   4. The per-thread SPSC ring: push N messages, flush, confirm zero drops.
//   5. Ring depth: within bounds, a power of two, and equal to
//      IHS_LOG_RING_CAPACITY when that asks for a legal depth. The CMake side
//      registers this binary twice, once with that variable set.
//
// The logging surface (ihs_log_* + the console/file sinks) is always compiled
// in; ENABLE_DLT only adds the DLT sink. So the same body runs in both
// variants — with DLT off the records simply fan out to the console sink. When
// libdlt is absent the DLT sink self-disables but logging still works via
// console, so the context stays valid either way.
//
// Prints "compat_smoke OK" on success and exits 0. A failing assertion aborts.

#include "logger.hpp"

#include "bridge.hpp"
#include "ring_registry.hpp"
#include "thread_ring.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>

namespace {

void check(bool cond, const char* what) {
  if (!cond) {
    std::fprintf(stderr, "compat_smoke: FAIL: %s\n", what);
    std::abort();
  }
}

}  // namespace

int main() {
  // 1. Lifecycle.
  IHS_LOGGING_START("SMOK", "compat smoke");

  // 2. Format-string portability. The context is valid regardless of DLT — the
  // console sink is always available — so the format paths always run.
  static IhsLogContext kCtx("SMOK", "smoke ctx");
  check(kCtx.is_valid(), "context should be valid (console sink always works)");

#if defined(IHS_HAS_FORMAT_TO_N)
  // C++20+: std::format backend — {} placeholders, compile-time checked.
  IHS_LOG_INFO(kCtx, "hello {}", "world");
  IHS_LOG_INFO(kCtx, "id={} flag={}", 42, true);
#else
  // C++17: snprintf fallback — printf-style placeholders.
  IHS_LOG_INFO(kCtx, "hello %s", "world");
  IHS_LOG_INFO(kCtx, "id=%d flag=%d", 42, 1);
#endif

  // 3. Direct log() path (pre-formatted, no format args). Acquire the bridge
  // handle directly — this harness compiles the bridge sources in, so the C++
  // API is available (the shell reaches the bridge only through the C ABI).
  const auto direct = ihs::dlt::DltBridge::instance().acquire_context(
      std::string_view{"SMOK"}, std::string_view{"smoke ctx"});
  const bool pushed = ihs::dlt::DltBridge::instance().log(
      direct, ihs::dlt::LogLevel::Info,
      std::string_view{"pre-formatted message"});
  check(pushed, "direct log push");

  // 4. Ring push stress: stay well under the ring depth so the worker drains
  // between bursts without any drops. Derived from ring_capacity() rather than
  // hardcoded, so the burst stays proportional when the depth is configured.
  auto& ring = ihs::dlt::RingRegistry::thread_local_ring();
  const std::uint64_t dropped_before = ring.dropped();
  const int burst = static_cast<int>(ihs::dlt::ring_capacity() / 2);

  for (int i = 0; i < burst; ++i) {
    IHS_LOG_INFO(kCtx,
#if defined(IHS_HAS_FORMAT_TO_N)
                 "burst {}",
#else
                 "burst %d",
#endif
                 i);
  }
  IHS_LOGGING_FLUSH();
  std::this_thread::sleep_for(std::chrono::milliseconds(25));

  check(ring.dropped() == dropped_before,
        "ring should not drop within capacity");

  // 5. Ring depth. The invariants the index mask depends on, plus the one that
  // makes the knob worth having: a depth that is read but not applied would
  // leave a diagnostic run shedding records while the operator believes it
  // raised the ceiling.
  const std::size_t capacity = ihs::dlt::ring_capacity();
  check(capacity >= ihs::dlt::kMinRingCapacity &&
            capacity <= ihs::dlt::kMaxRingCapacity,
        "ring capacity within bounds");
  check((capacity & (capacity - 1)) == 0, "ring capacity is a power of two");

  if (const char* asked = std::getenv("IHS_LOG_RING_CAPACITY");
      asked != nullptr && *asked != '\0') {
    const unsigned long long want = std::strtoull(asked, nullptr, 10);
    // Only a legal request must be honored exactly; anything else is clamped
    // or rounded on purpose, and the bounds checks above already cover it.
    if (want != 0 && (want & (want - 1)) == 0 &&
        want >= ihs::dlt::kMinRingCapacity &&
        want <= ihs::dlt::kMaxRingCapacity) {
      check(capacity == want, "configured ring capacity is honored");
    }
  }

  IHS_LOGGING_FLUSH();
  IHS_LOGGING_STOP();

#if defined(ENABLE_DLT)
  const int dlt = 1;
#else
  const int dlt = 0;
#endif
  std::printf("compat_smoke OK (cxx=%ld, ENABLE_DLT=%d)\n",
              static_cast<long>(__cplusplus), dlt);
  return 0;
}
