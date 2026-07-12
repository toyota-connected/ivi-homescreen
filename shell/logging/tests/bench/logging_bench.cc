// shell/logging/tests/bench/logging_bench.cc
//
// Producer-latency regression guard for the shell logging path. Measures the
// cost the CALLING thread pays per ihs::log call — fmt formatting + the C-ABI
// dispatch into the async ring — and asserts it stays within a bound of a
// same-machine reference so the guard is independent of the runner's speed.
//
// The reference is raw fmt formatting the identical line into a discarded stack
// buffer (pure format, no I/O) — the same fmt::format_to_n the logging.h shim
// runs, minus the C-ABI dispatch. Because both are measured on the same CPU in
// the same run, the RATIO is stable across machines; an absolute ceiling is a
// backstop.
// A future change that makes the producer synchronous, or adds a lock / heap
// allocation / syscall to the hot path, blows the ratio and fails the build.
//
// Modes:
//   logging_bench enabled   floor passes info -> measures format + dispatch
//   logging_bench filtered  IHS_LOG_LEVEL=off  -> measures the gate alone
//
// Exit code: 0 pass, 1 threshold exceeded, 2 setup error.
//
// Usage in CI: run both modes; each self-asserts.

#include "logging/logging.h"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

namespace {

struct Stat {
  double p50 = 0, p99 = 0, mean = 0;
};

Stat summarize(std::vector<long>& ns) {
  if (ns.empty()) {
    return {};
  }
  std::sort(ns.begin(), ns.end());
  double sum = 0;
  for (long v : ns) {
    sum += static_cast<double>(v);
  }
  auto at = [&](double p) {
    return static_cast<double>(
        ns[static_cast<size_t>(p * static_cast<double>(ns.size() - 1))]);
  };
  return {at(0.50), at(0.99), sum / static_cast<double>(ns.size())};
}

template <class F>
Stat measure(unsigned long long iters, F&& fn) {
  std::vector<long> lat;
  lat.reserve(iters / 64 + 1);
  for (unsigned long long i = 0; i < iters; ++i) {
    const auto t0 = Clock::now();
    fn(i);
    const auto t1 = Clock::now();
    if ((i & 63) == 0) {
      lat.push_back(static_cast<long>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count()));
    }
  }
  return summarize(lat);
}

int report(const char* mode,
           const Stat& ihs,
           const Stat& ref,
           double max_ratio,
           double abs_ceiling_ns) {
  const double ratio = ref.p50 > 0 ? ihs.p50 / ref.p50 : 0.0;
  std::printf(
      "[%s] ihs p50=%.0f p99=%.0f mean=%.1f ns | ref p50=%.0f | "
      "ratio=%.2f (max %.2f) | ceiling=%.0f ns\n",
      mode, ihs.p50, ihs.p99, ihs.mean, ref.p50, ratio, max_ratio,
      abs_ceiling_ns);
  if (ihs.p50 > abs_ceiling_ns) {
    std::printf("FAIL[%s]: ihs p50 %.0f ns exceeds absolute ceiling %.0f ns\n",
                mode, ihs.p50, abs_ceiling_ns);
    return 1;
  }
  if (ref.p50 > 0 && ratio > max_ratio) {
    std::printf(
        "FAIL[%s]: ihs/ref ratio %.2f exceeds %.2f — the producer hot "
        "path regressed relative to raw formatting\n",
        mode, ratio, max_ratio);
    return 1;
  }
  std::printf("PASS[%s]\n", mode);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "enabled";
  const unsigned long long iters =
      argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1'000'000ULL;
  const bool filtered = (mode == "filtered");

  // The IHS_LOG_LEVEL floor is read at ihs_log_start; set it before bring-up.
  if (filtered) {
    ::setenv("IHS_LOG_LEVEL", "off", 1);
  }
  // Keep the drained output off the terminal regardless of the sink.
  ::setenv("IHS_LOG_SINK", "console", 0);

  IHS_LOGGING_START("BNCH", "logging benchmark");
  ihs::log::info("warmup {}", 0);  // open the default context + warm the worker

  // Raw-fmt reference: format the identical line into a stack buffer and
  // discard. In filtered mode the format is gated out (mirroring ihs::log's
  // level gate), so the reference measures the same branch ihs pays.
  static volatile char ref_sink = 0;
  const bool ref_enabled = !filtered;

  const int frame = 4242;
  const double us = 3125.7;
  const int hz = 60;

  const Stat ihs = measure(iters, [&](unsigned long long i) {
    ihs::log::info("frame {} presented in {} us at {}Hz", frame + (int)(i & 7),
                   us, hz);
  });
  const Stat ref = measure(iters, [&](unsigned long long i) {
    if (ref_enabled) {
      char buf[256];
      const auto r = fmt::format_to_n(buf, sizeof(buf),
                                      "frame {} presented in {} us at {}Hz",
                                      frame + (int)(i & 7), us, hz);
      ref_sink = r.size > 0 ? buf[0] : char{0};
    }
  });

  IHS_LOGGING_FLUSH();
  IHS_LOGGING_STOP();

  // Enabled: format + FFI dispatch vs raw fmt formatting alone. The reference
  // is now pure fmt (no timestamp/level pattern), so the ratio reflects the
  // full cross-.so dispatch over bare formatting — ~1.5x locally, ~3x on a
  // shared CI runner (the FFI + ring path is noisier than pure ALU formatting).
  // 6.0x absorbs that variance while a lock/alloc/syscall on the hot path
  // (10x+, and caught by the absolute ns ceiling regardless) still fails.
  // Filtered: the gate alone vs the reference's gated-out branch — parity in
  // practice, 3.0x guards it.
  const int rc = filtered ? report("filtered", ihs, ref, 3.0, 250.0)
                          : report("enabled", ihs, ref, 6.0, 1500.0);
  return rc;
}
