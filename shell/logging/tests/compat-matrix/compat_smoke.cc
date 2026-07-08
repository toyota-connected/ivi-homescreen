// shell/logging/tests/compat-matrix/compat_smoke.cc
//
// Compat-matrix smoke test. Exercises the pieces that have to keep
// compiling across the C++17/20/23 toolchains:
//
//   1. The mux header's IHS_LOGGING_START/STOP/FLUSH lifecycle.
//   2. IHS_LOG_INFO format strings under both the std::format backend
//      ("{}") and the snprintf fallback ("%d"/"%s"), guarded by the same
//      IHS_HAS_* macros that switch the backend inside ihs::format_to.
//   3. Direct DltBridge::log() path (pre-formatted strings).
//   4. The per-thread SPSC ring: push N messages, flush, confirm zero
//      drops and no overflow.
//   5. IhsFlushWatchdog RAII construct/destruct.
//
// Prints "compat_smoke OK" on success and exits 0. Any failing assertion
// exits with a non-zero status via std::abort().

#include "logger.hpp"

#if defined(ENABLE_DLT)
#include "dlt/bridge.hpp"
#include "dlt/ring_registry.hpp"
#include "dlt/thread_ring.hpp"
#endif

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>

namespace {

#if defined(ENABLE_DLT)
void check(bool cond, const char* what) {
  if (!cond) {
    std::fprintf(stderr, "compat_smoke: FAIL: %s\n", what);
    std::abort();
  }
}
#endif

}  // namespace

int main() {
#if !defined(ENABLE_DLT)
  // spdlog-only sanity build: the mux macros must still parse and expand
  // to ((void)0). No bridge, no worker, no ring.
  IHS_LOGGING_START("SMOK", "compat smoke");
  IhsLogContext stub_ctx("SMOK", "stub");
  (void)stub_ctx.is_valid();
  IHS_LOG_INFO(stub_ctx, "stub {}", 1);
  IHS_LOG_ERROR(stub_ctx, "stub %d", 2);
  IHS_LOGGING_FLUSH();
  IHS_LOGGING_STOP();
  std::printf("compat_smoke OK (cxx=%ld, ENABLE_DLT=0)\n",
              static_cast<long>(__cplusplus));
  return 0;
#else
  // 1. Lifecycle.
  IHS_LOGGING_START("SMOK", "compat smoke");

  // 2. Format-string portability.
  static IhsLogContext kCtx("SMOK", "smoke ctx");
  check(kCtx.is_valid(), "context acquire");

#if defined(IHS_HAS_FORMAT_TO_N)
  // C++20+: std::format backend — {} placeholders, compile-time checked.
  IHS_LOG_INFO(kCtx, "hello {}", "world");
  IHS_LOG_INFO(kCtx, "id={} flag={}", 42, true);
#else
  // C++17: snprintf fallback — printf-style placeholders.
  IHS_LOG_INFO(kCtx, "hello %s", "world");
  IHS_LOG_INFO(kCtx, "id=%d flag=%d", 42, 1);
#endif

  // 3. Direct log() path (pre-formatted, no format args).
  const bool pushed = ihs::dlt::DltBridge::instance().log(
      kCtx.impl(), ihs::dlt::LogLevel::Info,
      std::string_view{"pre-formatted message"});
  check(pushed, "direct log push");

  // 4. Ring push stress: stay well under kRingCapacity so the worker
  // drains between bursts without any drops.
  auto& ring = ihs::dlt::RingRegistry::instance().thread_local_ring();
  const std::uint64_t dropped_before = ring.dropped();

  for (int i = 0; i < 128; ++i) {
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

  const std::uint64_t dropped_after = ring.dropped();
  check(dropped_after == dropped_before,
        "ring should not drop within capacity");

  // 5. Flush watchdog RAII — construct, let it tick once, destruct.
  {
    IhsFlushWatchdog wd(std::chrono::milliseconds(10));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }

  IHS_LOGGING_FLUSH();
  IHS_LOGGING_STOP();

  std::printf("compat_smoke OK (cxx=%ld)\n", static_cast<long>(__cplusplus));
  return 0;
#endif
}
