/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

// Unit test for the location Manager. Standalone (like registry_test): compiles
// manager.cc + registry.cc directly, registers fake measurement sources/filters
// through the public ABI, then drives measurements into the sink the manager
// hands each source and asserts the fused Position, the generation counter, the
// filter route, and source lifecycle.

#include "manager.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ihs/location.h"
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

// A fake source: start() records the sink so the test can push measurements as
// if the source's own thread produced them.
struct FakeSource {
  IhsMeasSink sink = nullptr;
  void* sink_ud = nullptr;
  int start_calls = 0;
  int stop_calls = 0;

  void Push(const IhsMeasurement& m) {
    if (sink != nullptr) {
      sink(sink_ud, &m);
    }
  }
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

IhsLocationSourceOps FakeSourceOps() {
  IhsLocationSourceOps ops{};
  ops.struct_size = sizeof(ops);
  ops.start = &FakeSourceStart;
  ops.stop = &FakeSourceStop;
  return ops;
}

// A position measurement with per-axis 1-sigma^2 variance.
IhsMeasurement PosMeas(double lat,
                       double lon,
                       uint64_t t,
                       double var_e = -1.0,
                       double var_n = -1.0) {
  IhsMeasurement m{};
  m.struct_size = sizeof(m);
  m.kind = IHS_MEAS_POSITION_LLA;
  m.t_monotonic_ns = t;
  m.value_count = 2;
  m.value[0] = lat;
  m.value[1] = lon;
  m.variance[0] = var_e;
  m.variance[1] = var_n;
  return m;
}

IhsMeasurement ScalarMeas(uint32_t kind, double v) {
  IhsMeasurement m{};
  m.struct_size = sizeof(m);
  m.kind = kind;
  m.value_count = 1;
  m.value[0] = v;
  m.variance[0] = -1.0;
  return m;
}

// A fake filter that stores the last position measurement and reports it.
struct FakeFilter {
  double lat = 0.0;
  double lon = 0.0;
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
    f->lat = m->value[0];
    f->lon = m->value[1];
  }
  ++f->updates;
}
int FakeFilterEstimate(void* inst, uint64_t t_ns, IhsPosition* out) {
  auto* f = static_cast<FakeFilter*>(inst);
  if (f->updates == 0) {
    return 0;
  }
  out->latitude = f->lat;
  out->longitude = f->lon;
  out->t_monotonic_ns = t_ns;
  out->mode = 3;
  return 1;
}

}  // namespace

int main() {
  using namespace ihs::location;

  // --- passthrough: fuse position + speed + heading into one fix ------------
  {
    FakeSource src;
    IhsLocationSourceOps ops = FakeSourceOps();
    Check(ihs_location_register_source("gnss.mgr", &ops, &src) == 1,
          "register source");

    Manager m({"gnss.mgr"}, "", "");
    Position pos;
    Check(!m.Latest(pos), "no fix before Start");
    Check(m.Start(), "Start succeeds");
    Check(src.start_calls == 1, "source started once");
    Check(!m.Latest(pos), "no fix before any measurement");
    Check(m.generation() == 0, "generation 0 before any fix");

    src.Push(PosMeas(48.75, -122.48, 1000, 9.0, 16.0));  // sigma 3 / 4 m
    Check(m.Latest(pos), "fix after a position measurement");
    Check(pos.latitude == 48.75 && pos.longitude == -122.48, "lat/lon fused");
    Check(pos.mode == 2, "2-component position => 2D mode");
    Check(pos.t_monotonic_ns == 1000, "timestamp carried");
    Check(std::abs(pos.sigma_e_m - 3.0) < 1e-9 &&
              std::abs(pos.sigma_n_m - 4.0) < 1e-9,
          "variance -> 1-sigma");
    Check(m.generation() == 1, "generation bumped once by the position");
    Check(!pos.has_bearing, "no bearing until a heading arrives");

    src.Push(ScalarMeas(IHS_MEAS_SPEED, 5.5));
    src.Push(ScalarMeas(IHS_MEAS_HEADING, 90.0));
    Check(m.Latest(pos), "still a fix");
    Check(pos.speed_mps == 5.5, "speed fused");
    Check(pos.has_bearing && pos.bearing_deg == 90.0, "heading fused");
    Check(m.generation() == 1,
          "speed/heading augment the same fix (no extra generation)");

    src.Push(PosMeas(48.76, -122.49, 2000));
    Check(m.generation() == 2, "next position bumps generation");
    Check(m.Latest(pos) && pos.latitude == 48.76, "latest position wins");

    m.Stop();
    Check(src.stop_calls == 1, "source stopped");
  }

  // --- bounded copy at the sink: a shorter (no-flags) measurement is still
  //     understood; a runt too small for the value block is dropped ----------
  {
    FakeSource src;
    IhsLocationSourceOps ops = FakeSourceOps();
    ihs_location_register_source("gnss.short", &ops, &src);
    Manager m({"gnss.short"}, "", "");
    m.Start();

    // A source built against a layout without the trailing flags field (a
    // shorter struct_size) is copied bounded and fully understood.
    IhsMeasurement shortm = PosMeas(33.0, 44.0, 7);
    shortm.struct_size = offsetof(IhsMeasurement, flags);
    src.Push(shortm);
    Position pos;
    Check(m.Latest(pos) && pos.latitude == 33.0,
          "a measurement without the trailing flags field is accepted");

    // A runt whose struct_size cannot even cover the value/variance block is
    // dropped, not read past.
    IhsMeasurement runt = PosMeas(55.0, 66.0, 8);
    runt.struct_size = offsetof(IhsMeasurement, value_count);
    const uint64_t before = m.generation();
    src.Push(runt);
    Check(m.generation() == before && m.Latest(pos) && pos.latitude == 33.0,
          "a runt measurement is dropped, leaving the prior fix intact");
    m.Stop();
  }

  // --- an unregistered source key is skipped, a registered one still binds --
  {
    FakeSource src;
    IhsLocationSourceOps ops = FakeSourceOps();
    ihs_location_register_source("gnss.present", &ops, &src);

    Manager m({"gnss.absent", "gnss.present"}, "", "");
    Check(m.Start(), "Start with a missing key");
    Check(src.start_calls == 1, "the registered source still started");
    src.Push(PosMeas(1.0, 2.0, 10));
    Position pos;
    Check(m.Latest(pos) && pos.latitude == 1.0, "fix from the present source");
    m.Stop();
  }

  // --- filter route: measurements go to update(), Latest() -> estimate() ----
  {
    FakeSource src;
    IhsLocationSourceOps sops = FakeSourceOps();
    ihs_location_register_source("gnss.filt", &sops, &src);

    IhsLocationFilterOps fops{};
    fops.struct_size = sizeof(fops);
    fops.create = &FakeFilterCreate;
    fops.destroy = &FakeFilterDestroy;
    fops.update = &FakeFilterUpdate;
    fops.estimate = &FakeFilterEstimate;
    Check(ihs_location_register_filter("kalman.mgr", &fops, nullptr) == 1,
          "register filter");

    Manager m({"gnss.filt"}, "kalman.mgr", "");
    Check(m.Start(), "Start with a filter");
    Position pos;
    Check(!m.Latest(pos), "filter reports no fix before any update");

    src.Push(PosMeas(51.5, -0.12, 42));
    Check(m.Latest(pos), "filter reports a fix after an update");
    Check(pos.latitude == 51.5 && pos.longitude == -0.12,
          "estimate carries the measurement");
    Check(pos.mode == 3, "estimate mode from the filter");
    m.Stop();  // destroys the filter instance (no leak)
  }

  // --- a configured-but-missing filter falls back to passthrough ------------
  {
    FakeSource src;
    IhsLocationSourceOps ops = FakeSourceOps();
    ihs_location_register_source("gnss.fb", &ops, &src);

    Manager m({"gnss.fb"}, "kalman.nope", "");
    Check(m.Start(), "Start with an unregistered filter key");
    src.Push(PosMeas(10.0, 20.0, 5));
    Position pos;
    Check(m.Latest(pos) && pos.latitude == 10.0,
          "missing filter falls back to passthrough, not a dead service");
    m.Stop();
  }

  if (g_failures == 0) {
    std::printf("manager_test: all %d checks passed\n", g_tests);
  } else {
    std::fprintf(stderr, "manager_test: %d/%d checks FAILED\n", g_failures,
                 g_tests);
  }
  return g_failures == 0 ? 0 : 1;
}
