/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

// Direct unit tests for the constant-velocity Kalman filter, driven through its
// ops table (create/update/estimate/destroy) without a Manager: the estimator
// math in isolation — seeding, noise reduction below raw, velocity recovery,
// prediction between fixes, outlier rejection (NIS gate), and coasting with a
// growing covariance through an outage.

#include "kalman_cv.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "ihs/location.h"
#include "track_gen.hpp"

namespace {

using ihs::location::KalmanCvFilterOps;
using ihs::location::test::AddNoise;
using ihs::location::test::MetersBetween;
using ihs::location::test::MetersPerDegLat;
using ihs::location::test::MetersPerDegLon;
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

constexpr double kLat0 = 48.0;
constexpr double kLon0 = -122.0;

IhsMeasurement PosMeas(double lat, double lon, double sigma_m, uint64_t t_ns) {
  IhsMeasurement m{};
  m.struct_size = sizeof(m);
  m.kind = IHS_MEAS_POSITION_LLA;
  m.t_monotonic_ns = t_ns;
  m.value_count = 2;
  m.value[0] = lat;
  m.value[1] = lon;
  m.variance[0] = sigma_m * sigma_m;
  m.variance[1] = sigma_m * sigma_m;
  return m;
}

// Position measurement with per-axis variance in the service order:
// variance[0] = east, variance[1] = north (see IhsPosition
// sigma_e_m/sigma_n_m).
IhsMeasurement PosMeasAniso(double lat,
                            double lon,
                            double var_e,
                            double var_n,
                            uint64_t t_ns) {
  IhsMeasurement m{};
  m.struct_size = sizeof(m);
  m.kind = IHS_MEAS_POSITION_LLA;
  m.t_monotonic_ns = t_ns;
  m.value_count = 2;
  m.value[0] = lat;
  m.value[1] = lon;
  m.variance[0] = var_e;
  m.variance[1] = var_n;
  return m;
}

uint64_t SecToNs(double s) {
  return static_cast<uint64_t>(s * 1e9);
}

// Lifecycle + seeding: no estimate before a measurement; the first measurement
// seeds the state so estimate() at that instant returns it.
void TestSeeding() {
  const IhsLocationFilterOps& ops = KalmanCvFilterOps();
  void* inst = ops.create(nullptr, nullptr);
  Check(inst != nullptr, "create returns an instance");

  IhsPosition out{};
  Check(ops.estimate(inst, 0, &out) == 0, "no estimate before any measurement");

  const IhsMeasurement m = PosMeas(kLat0, kLon0, 8.0, SecToNs(0.0));
  ops.update(inst, &m);
  Check(ops.estimate(inst, SecToNs(0.0), &out) == 1, "estimate after seed");
  Check(MetersBetween(out.latitude, out.longitude, kLat0, kLon0, kLat0) < 1.0,
        "seed estimate sits on the first measurement");
  Check(out.mode == 2, "2-component position reports a 2D fix");
  ops.destroy(inst);
}

// Noise reduction: filtered RMSE on a straight track must beat the raw fixes.
void TestBeatsRawStraight() {
  const auto truth =
      ihs::location::test::StraightTrack(kLat0, kLon0, 8.0, 0.0, 120, 1.0);
  Rng rng(0xC0FFEE);
  const double sigma = 8.0;
  const auto fixes = AddNoise(truth, sigma, rng);

  const IhsLocationFilterOps& ops = KalmanCvFilterOps();
  void* inst = ops.create(nullptr, nullptr);

  double sum_raw = 0.0;
  double sum_kf = 0.0;
  for (size_t i = 0; i < fixes.size(); ++i) {
    const IhsMeasurement m =
        PosMeas(fixes[i].lat, fixes[i].lon, sigma, fixes[i].t_ns);
    ops.update(inst, &m);
    IhsPosition out{};
    ops.estimate(inst, fixes[i].t_ns, &out);
    const double e_raw = MetersBetween(fixes[i].lat, fixes[i].lon, truth[i].lat,
                                       truth[i].lon, kLat0);
    const double e_kf = MetersBetween(out.latitude, out.longitude, truth[i].lat,
                                      truth[i].lon, kLat0);
    sum_raw += e_raw * e_raw;
    sum_kf += e_kf * e_kf;
  }
  const double rmse_raw =
      std::sqrt(sum_raw / static_cast<double>(fixes.size()));
  const double rmse_kf = std::sqrt(sum_kf / static_cast<double>(fixes.size()));
  std::printf("straight : rmse_raw=%.3f m  rmse_kf=%.3f m\n", rmse_raw,
              rmse_kf);
  Check(rmse_kf < rmse_raw * 0.8, "filtered RMSE materially below raw");
  ops.destroy(inst);
}

// Velocity recovery: a steady eastward track drives the estimated speed toward
// the truth and the course toward due east (90 deg).
void TestVelocityRecovery() {
  const double v_e = 12.0;  // m/s east
  const auto truth =
      ihs::location::test::StraightTrack(kLat0, kLon0, v_e, 0.0, 60, 1.0);
  Rng rng(0x5EED);
  const auto fixes = AddNoise(truth, 5.0, rng);

  const IhsLocationFilterOps& ops = KalmanCvFilterOps();
  void* inst = ops.create(nullptr, nullptr);
  IhsPosition out{};
  for (const auto& f : fixes) {
    const IhsMeasurement m = PosMeas(f.lat, f.lon, 5.0, f.t_ns);
    ops.update(inst, &m);
    ops.estimate(inst, f.t_ns, &out);
  }
  std::printf("velocity : speed=%.2f m/s (truth %.1f)  bearing=%.1f deg\n",
              out.speed_mps, v_e, out.bearing_deg);
  Check(std::abs(out.speed_mps - v_e) < 2.0, "recovers the true speed");
  Check(out.has_bearing != 0 && std::abs(out.bearing_deg - 90.0) < 10.0,
        "recovers an eastward course");
  ops.destroy(inst);
}

// Prediction between fixes: with a known eastward velocity, estimate() at a
// time beyond the last fix advances the position forward by v*dt.
void TestPredictionForward() {
  const double v_e = 10.0;
  const auto truth =
      ihs::location::test::StraightTrack(kLat0, kLon0, v_e, 0.0, 40, 1.0);
  // No noise: a clean velocity so the prediction is checkable to the meter.
  const IhsLocationFilterOps& ops = KalmanCvFilterOps();
  void* inst = ops.create(nullptr, nullptr);
  for (const auto& s : truth) {
    const IhsMeasurement m = PosMeas(s.lat, s.lon, 3.0, s.t_ns);
    ops.update(inst, &m);
  }
  const uint64_t last_t = truth.back().t_ns;
  IhsPosition at_last{};
  IhsPosition at_plus{};
  ops.estimate(inst, last_t, &at_last);
  ops.estimate(inst, last_t + SecToNs(2.0), &at_plus);
  const double advanced =
      MetersBetween(at_plus.latitude, at_plus.longitude, at_last.latitude,
                    at_last.longitude, kLat0);
  std::printf("predict  : advanced %.2f m over 2 s (expect ~%.1f)\n", advanced,
              v_e * 2.0);
  Check(std::abs(advanced - v_e * 2.0) < 3.0,
        "estimate predicts forward at the estimated velocity");
  ops.destroy(inst);
}

// Outlier rejection: a single large spike is gated out (the estimate barely
// moves), while the surrounding good fixes are tracked.
void TestOutlierRejection() {
  const IhsLocationFilterOps& ops = KalmanCvFilterOps();
  void* inst = ops.create(nullptr, nullptr);
  // Seed and settle on a stationary point.
  for (int i = 0; i < 20; ++i) {
    const IhsMeasurement m = PosMeas(kLat0, kLon0, 5.0, SecToNs(i));
    ops.update(inst, &m);
  }
  IhsPosition before{};
  ops.estimate(inst, SecToNs(20), &before);
  // One gross outlier ~500 m north (well past the gate).
  const double bad_lat = kLat0 + 500.0 / 111320.0;
  const IhsMeasurement spike = PosMeas(bad_lat, kLon0, 5.0, SecToNs(21));
  ops.update(inst, &spike);
  IhsPosition after{};
  ops.estimate(inst, SecToNs(21), &after);
  const double moved = MetersBetween(after.latitude, after.longitude,
                                     before.latitude, before.longitude, kLat0);
  std::printf("outlier  : estimate moved %.2f m for a 500 m spike\n", moved);
  Check(moved < 20.0, "a single gross outlier is gated out");
  ops.destroy(inst);
}

// Coasting: with no new measurements the estimate holds its trajectory and the
// reported accuracy grows monotonically (no confident wrong position).
void TestCoastingGrowsSigma() {
  const IhsLocationFilterOps& ops = KalmanCvFilterOps();
  void* inst = ops.create(nullptr, nullptr);
  const auto truth =
      ihs::location::test::StraightTrack(kLat0, kLon0, 6.0, 0.0, 20, 1.0);
  for (const auto& s : truth) {
    const IhsMeasurement m = PosMeas(s.lat, s.lon, 5.0, s.t_ns);
    ops.update(inst, &m);
  }
  const uint64_t last_t = truth.back().t_ns;
  IhsPosition a{};
  IhsPosition b{};
  ops.estimate(inst, last_t + SecToNs(1.0), &a);
  ops.estimate(inst, last_t + SecToNs(10.0), &b);
  std::printf("coast    : sigma_e %.2f m -> %.2f m over +9 s\n", a.sigma_e_m,
              b.sigma_e_m);
  Check(b.sigma_e_m > a.sigma_e_m && b.sigma_n_m > a.sigma_n_m,
        "reported accuracy degrades monotonically while coasting");
  ops.destroy(inst);
}

// Axis mapping of the measurement variance: variance[0] is the EAST error and
// variance[1] the NORTH error (the service convention). A fix offset equally in
// both axes but declared very uncertain in east and precise in north must pull
// the estimate north (trusted) far more than east (distrusted); a swapped
// mapping would do the opposite.
void TestAnisotropicVariance() {
  const IhsLocationFilterOps& ops = KalmanCvFilterOps();
  void* inst = ops.create(nullptr, nullptr);
  // Seed once so the state covariance is still wide (a tightly settled filter
  // would NIS-reject the offset below as an outlier, defeating the test).
  const IhsMeasurement seed = PosMeas(kLat0, kLon0, 2.0, SecToNs(0));
  ops.update(inst, &seed);
  // A modest +5 m offset in both axes — within the gate — but east variance
  // dominant and north variance tiny. (East must dominate the still-wide state
  // covariance, which the first predict inflates via the unknown initial
  // velocity, for the axes to separate cleanly.)
  const double off = 5.0;
  const double lat = kLat0 + off / MetersPerDegLat();
  const double lon = kLon0 + off / MetersPerDegLon(kLat0);
  const IhsMeasurement m =
      PosMeasAniso(lat, lon, /*var_e=*/1.0e6, /*var_n=*/0.01, SecToNs(1));
  ops.update(inst, &m);
  IhsPosition out{};
  ops.estimate(inst, SecToNs(1), &out);
  const double moved_n = (out.latitude - kLat0) * MetersPerDegLat();
  const double moved_e = (out.longitude - kLon0) * MetersPerDegLon(kLat0);
  std::printf("aniso    : moved north=%.2f m  east=%.2f m (offset 5 m each)\n",
              moved_n, moved_e);
  Check(moved_n > 3.0 && moved_e < 1.0,
        "the low-variance (north) axis is tracked, the high-variance (east) is "
        "not");
  ops.destroy(inst);
}

// A garbage (non-finite) measurement variance must be treated as unknown, not
// fed into S — otherwise S^-1 / NIS / K produce NaNs that corrupt the state.
void TestNonFiniteVariance() {
  const IhsLocationFilterOps& ops = KalmanCvFilterOps();
  void* inst = ops.create(nullptr, nullptr);
  const IhsMeasurement seed = PosMeas(kLat0, kLon0, 4.0, SecToNs(0));
  ops.update(inst, &seed);
  const double inf = std::numeric_limits<double>::infinity();
  const double lat = kLat0 + 3.0 / MetersPerDegLat();
  const IhsMeasurement bad = PosMeasAniso(lat, kLon0, inf, inf, SecToNs(1));
  ops.update(inst, &bad);
  IhsPosition out{};
  Check(ops.estimate(inst, SecToNs(1), &out) == 1,
        "estimate valid after an inf-variance measurement");
  Check(std::isfinite(out.latitude) && std::isfinite(out.longitude) &&
            std::isfinite(out.sigma_e_m) && std::isfinite(out.sigma_n_m),
        "non-finite variance does not corrupt the state with NaN");
  ops.destroy(inst);
}

}  // namespace

int main() {
  TestSeeding();
  TestBeatsRawStraight();
  TestVelocityRecovery();
  TestPredictionForward();
  TestOutlierRejection();
  TestCoastingGrowsSigma();
  TestAnisotropicVariance();
  TestNonFiniteVariance();

  if (g_failures == 0) {
    std::printf("kalman_cv_test: all %d checks passed\n", g_tests);
  } else {
    std::fprintf(stderr, "kalman_cv_test: %d/%d checks FAILED\n", g_failures,
                 g_tests);
  }
  return g_failures == 0 ? 0 : 1;
}
