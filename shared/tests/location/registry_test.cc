/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

// Unit test for the measurement source / filter registry. Standalone (like
// parse_tpv_test): compiles registry.cc directly, exercising both the internal
// Lookup* API and the public ihs_location_register_* entry points. Proves the
// table stores/replaces/rejects entries, that a shorter-struct_size caller is
// accepted with its missing callbacks read back NULL (the bounded-copy
// discipline), and that a measurement pushed to a source's sink and a filter's
// create/update/estimate round-trip through the stored ops.

#include "registry.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ihs/location.h"

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

// --- a fake source: start() records the sink and pushes one measurement ------

struct FakeSource {
  IhsMeasSink sink = nullptr;
  void* sink_ud = nullptr;
  int start_calls = 0;
  int stop_calls = 0;
};

int FakeSourceStart(void* ud, IhsMeasSink sink, void* sink_ud) {
  auto* s = static_cast<FakeSource*>(ud);
  s->sink = sink;
  s->sink_ud = sink_ud;
  ++s->start_calls;
  return 1;
}
void FakeSourceStop(void* ud) {
  ++static_cast<FakeSource*>(ud)->stop_calls;
}

// --- a captured sink ---------------------------------------------------------

struct SinkCapture {
  IhsMeasurement last{};
  int count = 0;
};
void CaptureSink(void* ud, const IhsMeasurement* m) {
  auto* c = static_cast<SinkCapture*>(ud);
  // Bounded copy, exactly what a real manager does: never read past the
  // producer's declared struct_size.
  std::memset(&c->last, 0, sizeof(c->last));
  const size_t n = m->struct_size < sizeof(IhsMeasurement)
                       ? m->struct_size
                       : sizeof(IhsMeasurement);
  std::memcpy(&c->last, m, n);
  ++c->count;
}

// --- a fake filter: sums position measurements -------------------------------

struct FakeFilter {
  double last_lat = 0.0;
  int updates = 0;
};

void* FakeFilterCreate(void* /*ud*/, const char* /*config*/) {
  return new FakeFilter();
}
void FakeFilterDestroy(void* inst) {
  delete static_cast<FakeFilter*>(inst);
}
void FakeFilterUpdate(void* inst, const IhsMeasurement* m) {
  auto* f = static_cast<FakeFilter*>(inst);
  if (m->kind == IHS_MEAS_POSITION_LLA && m->value_count >= 2) {
    f->last_lat = m->value[0];
  }
  ++f->updates;
}
int FakeFilterEstimate(void* inst, uint64_t t_ns, IhsPosition* out) {
  auto* f = static_cast<FakeFilter*>(inst);
  if (f->updates == 0) {
    return 0;
  }
  out->latitude = f->last_lat;
  out->t_monotonic_ns = t_ns;
  out->mode = 3;
  return 1;
}

}  // namespace

int main() {
  using namespace ihs::location;

  // --- source registration + lookup + a measurement through the sink ---------
  {
    FakeSource src;
    IhsLocationSourceOps ops{};
    ops.struct_size = sizeof(ops);
    ops.start = &FakeSourceStart;
    ops.stop = &FakeSourceStop;

    Check(ihs_location_register_source("gnss.fake", &ops, &src) == 1,
          "register source succeeds");

    SourceEntry entry;
    Check(LookupSource("gnss.fake", entry), "lookup registered source");
    Check(entry.user_data == &src, "source user_data round-trips");
    Check(entry.ops.start != nullptr && entry.ops.stop != nullptr,
          "source ops round-trip");

    SinkCapture cap;
    Check(entry.ops.start(entry.user_data, &CaptureSink, &cap) == 1,
          "start returns success");
    Check(src.start_calls == 1 && src.sink == &CaptureSink,
          "start recorded sink");

    IhsMeasurement m{};
    m.struct_size = sizeof(m);
    m.kind = IHS_MEAS_POSITION_LLA;
    m.t_monotonic_ns = 42;
    m.value_count = 2;
    m.value[0] = 48.75;
    m.value[1] = -122.48;
    src.sink(src.sink_ud, &m);  // source pushes into the captured sink
    Check(cap.count == 1, "sink received one measurement");
    Check(cap.last.value[0] == 48.75 && cap.last.value[1] == -122.48,
          "measurement values round-trip through sink");
    Check(cap.last.t_monotonic_ns == 42, "measurement timestamp round-trips");

    entry.ops.stop(entry.user_data);
    Check(src.stop_calls == 1, "stop reached the source");
  }

  // --- re-registering a key replaces the entry -------------------------------
  {
    FakeSource a;
    FakeSource b;
    IhsLocationSourceOps ops{};
    ops.struct_size = sizeof(ops);
    ops.start = &FakeSourceStart;
    ops.stop = &FakeSourceStop;

    ihs_location_register_source("gnss.dup", &ops, &a);
    ihs_location_register_source("gnss.dup", &ops, &b);
    SourceEntry entry;
    Check(LookupSource("gnss.dup", entry) && entry.user_data == &b,
          "re-register replaces the prior entry");
  }

  // --- filter registration + create/update/estimate round-trip ---------------
  {
    IhsLocationFilterOps ops{};
    ops.struct_size = sizeof(ops);
    ops.create = &FakeFilterCreate;
    ops.destroy = &FakeFilterDestroy;
    ops.update = &FakeFilterUpdate;
    ops.estimate = &FakeFilterEstimate;

    Check(ihs_location_register_filter("kalman.fake", &ops, nullptr) == 1,
          "register filter succeeds");

    FilterEntry entry;
    Check(LookupFilter("kalman.fake", entry), "lookup registered filter");

    void* inst = entry.ops.create(entry.user_data, nullptr);
    Check(inst != nullptr, "filter create returns an instance");

    IhsPosition out{};
    Check(entry.ops.estimate(inst, 0, &out) == 0,
          "estimate before any update reports no fix");

    IhsMeasurement m{};
    m.struct_size = sizeof(m);
    m.kind = IHS_MEAS_POSITION_LLA;
    m.value_count = 2;
    m.value[0] = 12.5;
    entry.ops.update(inst, &m);
    Check(entry.ops.estimate(inst, 99, &out) == 1,
          "estimate after update reports a fix");
    Check(out.latitude == 12.5 && out.t_monotonic_ns == 99,
          "filter estimate carries the measurement + query time");
    entry.ops.destroy(inst);
  }

  // --- rejections: NULL args and an under-sized ops --------------------------
  {
    IhsLocationSourceOps good{};
    good.struct_size = sizeof(good);
    good.start = &FakeSourceStart;
    good.stop = &FakeSourceStop;

    Check(ihs_location_register_source(nullptr, &good, nullptr) == 0,
          "reject NULL key");
    Check(ihs_location_register_source("k", nullptr, nullptr) == 0,
          "reject NULL ops");

    // struct_size that does not even reach start() — rejected, not accepted
    // with a garbage callback.
    IhsLocationSourceOps tiny{};
    tiny.struct_size = offsetof(IhsLocationSourceOps, start);
    tiny.start = &FakeSourceStart;
    Check(ihs_location_register_source("gnss.tiny", &tiny, nullptr) == 0,
          "reject ops too small to hold start()");

    // A source with room for start() but a NULL start() is unusable — rejected.
    IhsLocationSourceOps no_start{};
    no_start.struct_size = sizeof(no_start);
    no_start.start = nullptr;
    no_start.stop = &FakeSourceStop;
    Check(ihs_location_register_source("gnss.nostart", &no_start, nullptr) == 0,
          "reject a source with a NULL start()");

    SourceEntry absent;
    Check(!LookupSource("gnss.absent", absent),
          "lookup of an unregistered key fails");
  }

  // --- a filter missing a mandatory callback is rejected ---------------------
  {
    IhsLocationFilterOps ops{};
    ops.struct_size = sizeof(ops);
    ops.create = &FakeFilterCreate;
    ops.destroy = &FakeFilterDestroy;
    ops.update = &FakeFilterUpdate;
    ops.estimate = nullptr;  // the mandatory estimator is missing
    Check(ihs_location_register_filter("kalman.noest", &ops, nullptr) == 0,
          "reject a filter with a NULL estimate()");

    // destroy() is optional: a filter that provides create/update/estimate but
    // no destroy() is still usable and must be accepted.
    IhsLocationFilterOps no_destroy{};
    no_destroy.struct_size = sizeof(no_destroy);
    no_destroy.create = &FakeFilterCreate;
    no_destroy.destroy = nullptr;
    no_destroy.update = &FakeFilterUpdate;
    no_destroy.estimate = &FakeFilterEstimate;
    Check(ihs_location_register_filter("kalman.nodestroy", &no_destroy,
                                       nullptr) == 1,
          "accept a filter with no destroy()");
  }

  // --- bounded copy: a shorter-struct_size ops that omits stop() reads back
  //     NULL rather than being over-read past the caller's object -------------
  {
    IhsLocationSourceOps partial{};
    // Declare a size that covers start() but stops before stop(). stop is set
    // to a real callback, but it lies past the declared struct_size, so the
    // registry must NOT copy it.
    partial.struct_size = offsetof(IhsLocationSourceOps, stop);
    partial.start = &FakeSourceStart;
    partial.stop = &FakeSourceStop;

    Check(ihs_location_register_source("gnss.partial", &partial, nullptr) == 1,
          "accept ops that reaches start() but not stop()");
    SourceEntry entry;
    Check(LookupSource("gnss.partial", entry), "lookup partial source");
    Check(entry.ops.start == &FakeSourceStart, "partial: start copied");
    Check(entry.ops.stop == nullptr,
          "partial: stop past struct_size read back NULL, not the bogus value");
  }

  if (g_failures == 0) {
    std::printf("registry_test: all %d checks passed\n", g_tests);
  } else {
    std::fprintf(stderr, "registry_test: %d/%d checks FAILED\n", g_failures,
                 g_tests);
  }
  return g_failures == 0 ? 0 : 1;
}
