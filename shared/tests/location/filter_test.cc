/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

// Location filter harness. Generates a synthetic ground-truth track, adds
// Gaussian measurement noise, feeds the noisy fixes through a Manager, and
// compares filtered RMSE to raw RMSE — the metric a real estimator must beat.
//
// Today the only "filter" is the Manager's passthrough (no filter registered),
// so this reports filtered == raw: the baseline increment 4's kalman.cv must
// improve on. The scenarios (straight, stationary, outage) and their reports
// are built now so the improvement is provable in CI rather than eyeballed.
//
// One seam is deliberately deferred to the filter increment. This harness reads
// the estimate with Manager::Latest(), which for a real filter evaluates
// estimate() at the wall clock (MonotonicNs()), not at the synthetic
// measurement time stamped into each pushed measurement. The passthrough is
// time-independent, so the baseline here is exact; but a constant-velocity
// filter would extrapolate across the gap between the synthetic timeline (based
// at 0) and the wall clock and produce nonsense. Wiring a real filter therefore
// also needs the estimate sampled on the synthetic timeline (a
// time-parameterized read) — that lands with the filter, where it is testable.

#include "manager.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "ihs/location.h"
#include "location.hpp"
#include "track_gen.hpp"

namespace {

using ihs::location::Manager;
using ihs::location::Position;
using ihs::location::test::AddNoise;
using ihs::location::test::MetersBetween;
using ihs::location::test::NoisyFix;
using ihs::location::test::Rng;
using ihs::location::test::TruthSample;

int g_tests = 0;
int g_failures = 0;
void Check(bool cond, const char* what) {
  ++g_tests;
  if (!cond) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

// A fake measurement source: start() records the sink so the harness can push
// the synthetic measurements as if a real source produced them.
struct FakeSource {
  IhsMeasSink sink = nullptr;
  void* sink_ud = nullptr;
};
int FakeStart(void* ud, IhsMeasSink sink, void* sink_ud) {
  auto* s = static_cast<FakeSource*>(ud);
  s->sink = sink;
  s->sink_ud = sink_ud;
  return 1;
}
void FakeStop(void* /*ud*/) {}

// The registry keeps a raw pointer to a source's user-data for the process
// lifetime (there is no unregister), so the source must outlive every Manager
// that could bind it. Own the fakes here rather than in a stack local, whose
// address would dangle in the registry once RunFilter returns.
std::vector<std::unique_ptr<FakeSource>>& SourceStore() {
  static std::vector<std::unique_ptr<FakeSource>> store;
  return store;
}

// Feed the noisy fixes through a Manager bound to @filter_key (empty = the
// built-in passthrough), sampling the Manager's estimate after each. @withhold
// pairs (first, count): skip pushing `count` fixes starting at index `first`,
// to model a measurement outage — the estimate is still sampled, so a coasting
// filter shows up as a moving estimate during the gap.
std::vector<Position> RunFilter(const std::vector<NoisyFix>& fixes,
                                const std::string& filter_key,
                                size_t withhold_first = 0,
                                size_t withhold_count = 0) {
  FakeSource& src = *SourceStore().emplace_back(std::make_unique<FakeSource>());
  IhsLocationSourceOps ops{};
  ops.struct_size = sizeof(ops);
  ops.start = &FakeStart;
  ops.stop = &FakeStop;
  Check(ihs_location_register_source("gnss.harness", &ops, &src) == 1,
        "fake source registered");

  Manager m({"gnss.harness"}, filter_key, "");
  Check(m.Start(), "Manager started");
  // Prove the Manager bound the source: FakeStart ran and handed us the sink.
  // Without this the loop below would silently exercise the no-fix path.
  Check(src.sink != nullptr, "Manager bound the fake source");

  std::vector<Position> est;
  est.reserve(fixes.size());
  for (size_t i = 0; i < fixes.size(); ++i) {
    const bool withheld = withhold_count > 0 && i >= withhold_first &&
                          i < withhold_first + withhold_count;
    if (!withheld && src.sink != nullptr) {
      const NoisyFix& f = fixes[i];
      IhsMeasurement mm{};
      mm.struct_size = sizeof(mm);
      mm.kind = IHS_MEAS_POSITION_LLA;
      mm.t_monotonic_ns = f.t_ns;
      mm.value_count = 2;
      mm.value[0] = f.lat;
      mm.value[1] = f.lon;
      mm.variance[0] = f.sigma_m * f.sigma_m;
      mm.variance[1] = f.sigma_m * f.sigma_m;
      src.sink(src.sink_ud, &mm);
    }
    Position p;
    if (!m.Latest(p)) {
      p = Position{};  // no fix yet
    }
    est.push_back(p);
  }
  m.Stop();
  return est;
}

// RMSE (meters) of the noisy fixes vs truth, over [from, to). Total: clamps to
// the shorter vector and returns 0 over an empty window.
double RmseRaw(const std::vector<NoisyFix>& fixes,
               const std::vector<TruthSample>& truth,
               double ref_lat,
               size_t from,
               size_t to) {
  const size_t end = std::min({to, fixes.size(), truth.size()});
  double sum = 0.0;
  size_t n = 0;
  for (size_t i = from; i < end; ++i) {
    const double d = MetersBetween(fixes[i].lat, fixes[i].lon, truth[i].lat,
                                   truth[i].lon, ref_lat);
    sum += d * d;
    ++n;
  }
  return n == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(n));
}

// RMSE (meters) of the estimates vs truth, over [from, to).
double RmseEst(const std::vector<Position>& est,
               const std::vector<TruthSample>& truth,
               double ref_lat,
               size_t from,
               size_t to) {
  double sum = 0.0;
  size_t n = 0;
  for (size_t i = from; i < to && i < truth.size(); ++i) {
    const double d = MetersBetween(est[i].latitude, est[i].longitude,
                                   truth[i].lat, truth[i].lon, ref_lat);
    sum += d * d;
    ++n;
  }
  return n == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(n));
}

}  // namespace

int main() {
  const double kLat0 = 48.0;
  const double kLon0 = -122.0;
  const double kSigma = 8.0;  // per-axis measurement noise, meters

  // --- straight track: raw vs filtered RMSE --------------------------------
  {
    const auto truth =
        ihs::location::test::StraightTrack(kLat0, kLon0, 5.0, 3.0, 120, 1.0);
    Rng rng(0x1234ABCD);
    const auto fixes = AddNoise(truth, kSigma, rng);
    const auto est = RunFilter(fixes, "" /* passthrough */);

    const double raw = RmseRaw(fixes, truth, kLat0, 0, truth.size());
    const double filt = RmseEst(est, truth, kLat0, 0, truth.size());
    std::printf("straight : raw=%.3f m  filtered=%.3f m  (sigma=%.1f)\n", raw,
                filt, kSigma);

    // Harness sanity: per-axis sigma 8 m gives a 2D RMSE ~ sigma*sqrt(2).
    Check(raw > kSigma && raw < kSigma * 2.0,
          "raw RMSE is in the expected band for the noise sigma");
    // Baseline: the passthrough republishes the last measurement, so filtered
    // equals raw. increment 4's kalman.cv must drive filtered materially below.
    Check(
        std::abs(filt - raw) < 0.05,
        "passthrough filtered RMSE equals raw (no improvement) — the baseline");
  }

  // --- stationary: does the estimate settle? -------------------------------
  {
    const auto truth =
        ihs::location::test::StationaryTrack(kLat0, kLon0, 120, 1.0);
    Rng rng(0x55AA55AA);
    const auto fixes = AddNoise(truth, kSigma, rng);
    const auto est = RunFilter(fixes, "" /* passthrough */);

    const double raw_full = RmseRaw(fixes, truth, kLat0, 0, truth.size());
    // Compare like windows: late-window filtered vs late-window raw. For a real
    // filter the filtered value collapses well below raw; for the passthrough
    // it equals raw exactly (the estimate is the last measurement), so a tight
    // tolerance holds and a regression that perturbed the passthrough here
    // would trip it — where the earlier full-window-vs-late-window compare
    // would not.
    const size_t late = 60;
    const double raw_late = RmseRaw(fixes, truth, kLat0, late, truth.size());
    const double filt_late = RmseEst(est, truth, kLat0, late, truth.size());
    std::printf(
        "stationary: raw=%.3f m  raw(late)=%.3f m  filtered(late)=%.3f m\n",
        raw_full, raw_late, filt_late);
    Check(std::abs(filt_late - raw_late) < 0.05,
          "passthrough does not collapse the stationary variance (baseline)");
  }

  // --- outage: withhold measurements mid-track -----------------------------
  {
    const auto truth =
        ihs::location::test::StraightTrack(kLat0, kLon0, 6.0, 0.0, 120, 1.0);
    Rng rng(0x0F0F0F0F);
    const auto fixes = AddNoise(truth, kSigma, rng);
    // Withhold 10 s (samples 50..59).
    const auto est = RunFilter(fixes, "" /* passthrough */, 50, 10);

    // During the gap the passthrough holds the last fix (no coasting): its
    // error grows only because the vehicle keeps moving away from that stale
    // point.
    const double gap = RmseEst(est, truth, kLat0, 50, 60);
    std::printf(
        "outage   : passthrough error across the 10 s gap=%.3f m "
        "(coasting filter must beat this)\n",
        gap);
    Check(gap > 0.0, "harness produced an estimate across the outage");
  }

  if (g_failures == 0) {
    std::printf("filter_test: all %d checks passed\n", g_tests);
  } else {
    std::fprintf(stderr, "filter_test: %d/%d checks FAILED\n", g_failures,
                 g_tests);
  }
  return g_failures == 0 ? 0 : 1;
}
