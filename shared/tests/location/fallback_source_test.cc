/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

// Unit test for the event-driven gpsd-primary / geoclue-fallback selection.
// Standalone: compiles fallback_source.cc directly and drives fake event
// sources, asserting which fixes reach the outer sink as the primary produces,
// goes silent past the staleness window, and recovers. Timestamps are supplied
// explicitly (they are the monotonic arrival times the selection compares), so
// the test is deterministic without real clocks or threads.

#include "fallback_source.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>

#include "location.hpp"

namespace {

int g_tests = 0;
int g_failures = 0;
void Check(bool cond, const char* what) {
  ++g_tests;
  if (!cond) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

// A fake event source: records the sink the combiner installs, and Emit()
// pushes a fix through it as if the worker thread had.
struct FakeEventSource : ihs::location::IEventSource {
  ihs::location::FixSink on_fix;
  void SetOnFix(ihs::location::FixSink cb) override { on_fix = std::move(cb); }
  bool Start() override { return true; }
  void Stop() override {}
  void Emit(const ihs::location::Position& p) {
    if (on_fix) {
      on_fix(p);
    }
  }
};

constexpr uint64_t kSec = 1'000'000'000ULL;

ihs::location::Position PosAt(double lat, uint64_t t_ns) {
  ihs::location::Position p;
  p.latitude = lat;
  p.longitude = 10.0;
  p.mode = 3;
  p.t_monotonic_ns = t_ns;
  return p;
}

}  // namespace

int main() {
  using namespace ihs::location;

  auto* prim = new FakeEventSource();
  auto* fb = new FakeEventSource();
  FallbackSource fs(std::unique_ptr<IEventSource>(prim),
                    std::unique_ptr<IEventSource>(fb), 5 * kSec);

  double last_lat = 0.0;
  int count = 0;
  fs.SetOnFix([&](const Position& p) {
    last_lat = p.latitude;
    ++count;
  });
  fs.Start();  // installs the combiner's handlers onto prim/fb

  // Before any primary fix, the fallback fills in.
  fb->Emit(PosAt(1.0, kSec / 10));
  Check(count == 1 && last_lat == 1.0,
        "fallback forwarded before the primary is ever seen");

  // Primary produces: it is authoritative and forwarded.
  prim->Emit(PosAt(2.0, 1 * kSec));
  Check(count == 2 && last_lat == 2.0, "primary fix forwarded");

  // Fallback while the primary is fresh (0.5 s later) is dropped.
  fb->Emit(PosAt(9.0, 1 * kSec + kSec / 2));
  Check(count == 2 && last_lat == 2.0,
        "fallback dropped while the primary is fresh");

  // Fallback after the primary has been silent > 5 s is forwarded.
  fb->Emit(PosAt(3.0, 7 * kSec));
  Check(count == 3 && last_lat == 3.0,
        "fallback forwarded once the primary is stale");

  // Primary recovers: authoritative again.
  prim->Emit(PosAt(4.0, 8 * kSec));
  Check(count == 4 && last_lat == 4.0, "primary reasserts on recovery");

  // Fallback right after recovery is dropped again.
  fb->Emit(PosAt(9.0, 8 * kSec + kSec / 2));
  Check(count == 4 && last_lat == 4.0,
        "fallback dropped again once the primary is fresh");

  fs.Stop();

  if (g_failures == 0) {
    std::printf("fallback_source_test: all %d checks passed\n", g_tests);
  } else {
    std::fprintf(stderr, "fallback_source_test: %d/%d FAILED\n", g_failures,
                 g_tests);
  }
  return g_failures == 0 ? 0 : 1;
}
