// shell/logging/tests/load/dlt_load_test.cc
//
// Throughput / drop / latency load test for the DLT bridge. Drives N producer
// threads through DltBridge::log() for a fixed duration (optionally rate-
// limited), then reports:
//
//   - ingest throughput (messages/second across all producers)
//   - ring drops (producer faster than the worker can drain)
//   - degraded drops (bridge self-disabled)
//   - delivered count (read back from the stub libdlt)
//   - per-call log() latency p50 / p99 / max
//   - RSS delta (memory stability)
//
// Requires a libdlt to emit into. For CI, build shell/logging/tests/load
// (which also builds dlt_stub -> libdlt.so.2) and run with
// IHS_DLT_LIBRARY pointed at the stub; without it the bridge self-disables and
// the test reports that and exits 2.
//
// Usage: dlt_load_test [threads=4] [seconds=5] [rate_per_thread=0(max)]
#include "logger.hpp"

#include "dlt/bridge.hpp"
#include "dlt/libdlt_loader.hpp"
#include "dlt/ring_registry.hpp"
#include "dlt/thread_ring.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

long read_vmrss_kb() {
  std::FILE* f = std::fopen("/proc/self/status", "re");
  if (f == nullptr) {
    return -1;
  }
  char line[256];
  long kb = -1;
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    if (std::sscanf(line, "VmRSS: %ld kB", &kb) == 1) {
      break;
    }
  }
  std::fclose(f);
  return kb;
}

// Read the stub's delivered count from the already-loaded library.
unsigned long long stub_emit_count() {
  const char* path = std::getenv("IHS_DLT_LIBRARY");
  if (path == nullptr) {
    return 0;
  }
  void* h = ::dlopen(path, RTLD_NOLOAD | RTLD_NOW);
  if (h == nullptr) {
    return 0;
  }
  auto fn = reinterpret_cast<unsigned long long (*)()>(
      ::dlsym(h, "dlt_stub_emit_count"));
  const unsigned long long n = fn ? fn() : 0;
  ::dlclose(h);  // RTLD_NOLOAD dlopen took a ref; drop it
  return n;
}

double percentile(std::vector<long>& v, double p) {
  if (v.empty()) {
    return 0.0;
  }
  std::sort(v.begin(), v.end());
  const auto idx = static_cast<std::size_t>(p * (v.size() - 1));
  return static_cast<double>(v[idx]);
}

}  // namespace

int main(int argc, char** argv) {
  const int threads = argc > 1 ? std::atoi(argv[1]) : 4;
  const int seconds = argc > 2 ? std::atoi(argv[2]) : 5;
  const long rate = argc > 3 ? std::atol(argv[3]) : 0;  // per thread, 0 = max

  const long rss_before = read_vmrss_kb();

  IHS_LOGGING_START("LOAD", "dlt load test");
  static IhsLogContext ctx("LOAD", "load ctx");
  if (!ctx.is_valid()) {
    std::fprintf(stderr,
                 "bridge disabled: set IHS_DLT_LIBRARY to a libdlt (or the "
                 "stub) so the emit path runs\n");
    return 2;
  }

  constexpr std::string_view kPayload =
      "load message payload, roughly forty bytes..";  // ~44 B

  std::atomic<bool> go{false};
  std::vector<unsigned long long> sent(static_cast<std::size_t>(threads), 0);
  std::vector<std::vector<long>> lat(static_cast<std::size_t>(threads));

  auto producer = [&](int tid) {
    while (!go.load(std::memory_order_acquire)) {
    }
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    const auto period = rate > 0
                            ? std::chrono::nanoseconds(1'000'000'000LL / rate)
                            : std::chrono::nanoseconds(0);
    auto next = Clock::now();
    unsigned long long n = 0;
    while (Clock::now() < deadline) {
      const auto t0 = Clock::now();
      ihs::dlt::DltBridge::instance().log(ctx.impl(), ihs::dlt::LogLevel::Info,
                                          kPayload);
      const auto t1 = Clock::now();
      ++n;
      if ((n & 0x3FF) == 0) {  // sample every 1024th call
        lat[static_cast<std::size_t>(tid)].push_back(static_cast<long>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count()));
      }
      if (rate > 0) {
        next += period;
        std::this_thread::sleep_until(next);
      }
    }
    sent[static_cast<std::size_t>(tid)] = n;
  };

  std::vector<std::thread> pool;
  pool.reserve(static_cast<std::size_t>(threads));
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back(producer, t);
  }
  const auto start = Clock::now();
  go.store(true, std::memory_order_release);
  for (auto& th : pool) {
    th.join();
  }
  const auto elapsed =
      std::chrono::duration<double>(Clock::now() - start).count();

  // Let the worker drain the backlog before tearing down.
  IHS_LOGGING_FLUSH();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  IHS_LOGGING_STOP();

  unsigned long long ring_drops = 0;
  for (ihs::dlt::ThreadRing* r = ihs::dlt::RingRegistry::instance().head();
       r != nullptr; r = r->next) {
    ring_drops += r->dropped();
  }
  const unsigned long long degraded =
      ihs::dlt::LibDltLoader::instance().degraded_drops();
  const unsigned long long delivered = stub_emit_count();

  unsigned long long total_sent = 0;
  std::vector<long> all_lat;
  for (int t = 0; t < threads; ++t) {
    total_sent += sent[static_cast<std::size_t>(t)];
    auto& v = lat[static_cast<std::size_t>(t)];
    all_lat.insert(all_lat.end(), v.begin(), v.end());
  }

  const long rss_after = read_vmrss_kb();

  std::printf("=== dlt load test ===\n");
  std::printf("threads=%d duration=%ds rate/thread=%s\n", threads, seconds,
              rate > 0 ? std::to_string(rate).c_str() : "max");
  std::printf("ingest:     %llu msgs in %.2fs = %.0f msg/s\n", total_sent,
              elapsed, static_cast<double>(total_sent) / elapsed);
  std::printf("delivered:  %llu (%.2f%%)\n", delivered,
              total_sent ? 100.0 * static_cast<double>(delivered) /
                               static_cast<double>(total_sent)
                         : 0.0);
  std::printf("ring drops: %llu (%.2f%%)   degraded drops: %llu\n", ring_drops,
              total_sent ? 100.0 * static_cast<double>(ring_drops) /
                               static_cast<double>(total_sent)
                         : 0.0,
              degraded);
  std::printf("log() latency (ns): p50=%.0f p99=%.0f max=%.0f (n=%zu)\n",
              percentile(all_lat, 0.50), percentile(all_lat, 0.99),
              percentile(all_lat, 1.0), all_lat.size());
  std::printf("RSS: %ld -> %ld kB (delta %+ld kB)\n", rss_before, rss_after,
              rss_after - rss_before);

  // Cross-check: everything the rings accepted should have been delivered.
  const unsigned long long accepted = total_sent - ring_drops;
  if (delivered + 0 != accepted) {
    std::printf(
        "NOTE: delivered(%llu) != accepted(%llu) — worker backlog "
        "or timing\n",
        delivered, accepted);
  }
  return 0;
}
