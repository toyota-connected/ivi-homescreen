/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

// Unit tests for the CTRV extended Kalman filter. The load-bearing one is the
// numerical Jacobian check: an EKF's analytic Jacobian is its most error-prone
// piece, so it is finite-differenced against the predict function. The rest are
// behavioral: seeding, beating the raw fixes, beating the constant-velocity
// filter on a turn (the reason CTRV exists), the scalar speed/yaw-rate updates
// a CAN source drives, and a growing covariance while coasting.

#include "kalman_ctrv.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "ihs/location.h"
#include "kalman_cv.hpp"
#include "track_gen.hpp"

namespace {

using ihs::location::KalmanCtrvFilterOps;
using ihs::location::KalmanCtrvJacobianForTest;
using ihs::location::KalmanCtrvPredictForTest;
using ihs::location::KalmanCvFilterOps;
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

constexpr double kLat0 = 48.0;
constexpr double kLon0 = -122.0;

uint64_t SecToNs(double s) {
  return static_cast<uint64_t>(s * 1e9);
}

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

IhsMeasurement ScalarMeas(uint32_t kind, double v, double var, uint64_t t_ns) {
  IhsMeasurement m{};
  m.struct_size = sizeof(m);
  m.kind = kind;
  m.t_monotonic_ns = t_ns;
  m.value_count = 1;
  m.value[0] = v;
  m.variance[0] = var;
  return m;
}

// RMSE (meters) of a filter's estimate vs truth, feeding each noisy fix and
// sampling the estimate at that fix's time.
double RmseThroughOps(const IhsLocationFilterOps& ops,
                      const std::vector<TruthSample>& truth,
                      const std::vector<NoisyFix>& fixes) {
  void* inst = ops.create(nullptr, nullptr);
  double sum = 0.0;
  std::size_t n = 0;
  for (std::size_t i = 0; i < fixes.size(); ++i) {
    const IhsMeasurement m =
        PosMeas(fixes[i].lat, fixes[i].lon, fixes[i].sigma_m, fixes[i].t_ns);
    ops.update(inst, &m);
    IhsPosition out{};
    if (ops.estimate(inst, fixes[i].t_ns, &out) == 1) {
      const double d = MetersBetween(out.latitude, out.longitude, truth[i].lat,
                                     truth[i].lon, kLat0);
      sum += d * d;
      ++n;
    }
  }
  ops.destroy(inst);
  return n == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(n));
}

// The load-bearing test: the analytic Jacobian must match a central-difference
// of the predict function, at both turning and near-straight states.
void TestJacobianNumeric() {
  const double states[][5] = {
      {10.0, 20.0, 0.5, 12.0, 0.20},   // turning left
      {-5.0, 8.0, 2.5, 9.0, -0.35},    // turning right
      {3.0, -4.0, 1.0, 15.0, 1.0e-6},  // near-straight (limit branch)
      {0.0, 0.0, 0.0, 0.0, 0.0},       // at rest
      {1.0, 1.0, -1.2, 20.0, 0.05},    // fast, gentle turn
  };
  const double dt = 0.1;
  bool ok = true;
  for (const auto& s : states) {
    double ja[25];
    KalmanCtrvJacobianForTest(s, dt, ja);
    for (int j = 0; j < 5; ++j) {
      double xp[5];
      double xm[5];
      for (int c = 0; c < 5; ++c) {
        xp[c] = s[c];
        xm[c] = s[c];
      }
      const double h = 1.0e-6 * (std::fabs(s[j]) + 1.0e-3);
      xp[j] += h;
      xm[j] -= h;
      double fp[5];
      double fm[5];
      KalmanCtrvPredictForTest(xp, dt, fp);
      KalmanCtrvPredictForTest(xm, dt, fm);
      for (int i = 0; i < 5; ++i) {
        const double num = (fp[i] - fm[i]) / (2.0 * h);
        if (std::fabs(num - ja[i * 5 + j]) > 1.0e-4) {
          ok = false;
        }
      }
    }
  }
  Check(ok, "analytic Jacobian matches the numerical derivative");
}

void TestSeeding() {
  const IhsLocationFilterOps& ops = KalmanCtrvFilterOps();
  void* inst = ops.create(nullptr, nullptr);
  IhsPosition out{};
  Check(ops.estimate(inst, 0, &out) == 0, "no estimate before a measurement");
  const IhsMeasurement m = PosMeas(kLat0, kLon0, 8.0, SecToNs(0.0));
  ops.update(inst, &m);
  Check(ops.estimate(inst, SecToNs(0.0), &out) == 1, "estimate after seed");
  Check(MetersBetween(out.latitude, out.longitude, kLat0, kLon0, kLat0) < 1.0,
        "seed estimate sits on the first measurement");
  ops.destroy(inst);
}

void TestBeatsRawStraight() {
  const auto truth =
      ihs::location::test::StraightTrack(kLat0, kLon0, 10.0, 0.0, 120, 1.0);
  Rng rng(0xCC77);
  const double sigma = 8.0;
  const auto fixes = AddNoise(truth, sigma, rng);
  const double rmse = RmseThroughOps(KalmanCtrvFilterOps(), truth, fixes);
  std::printf("straight : rmse_ctrv=%.3f m (sigma=%.1f)\n", rmse, sigma);
  Check(rmse < sigma * 1.2, "CTRV beats raw on a straight track");
}

// The reason CTRV exists: on a constant-radius turn it should track the corner
// the straight-line CV filter lags.
void TestBeatsCvOnTurn() {
  const auto truth = ihs::location::test::ConstantTurnTrack(
      kLat0, kLon0, /*speed_mps=*/15.0, /*yaw_rate_dps=*/12.0,
      /*heading0_deg=*/0.0, /*n=*/120, /*dt_s=*/0.5);
  Rng rng(0x7717);
  const auto fixes = AddNoise(truth, 6.0, rng);
  const double rmse_ctrv = RmseThroughOps(KalmanCtrvFilterOps(), truth, fixes);
  const double rmse_cv = RmseThroughOps(KalmanCvFilterOps(), truth, fixes);
  std::printf("turn     : rmse_ctrv=%.3f m  rmse_cv=%.3f m\n", rmse_ctrv,
              rmse_cv);
  Check(rmse_ctrv < rmse_cv, "CTRV tracks a turn better than CV");
}

// A CAN ground-speed measurement is a scalar update on v: feeding it drives the
// reported speed toward the measured value.
void TestSpeedUpdate() {
  const IhsLocationFilterOps& ops = KalmanCtrvFilterOps();
  void* inst = ops.create(nullptr, nullptr);
  // Seed with a position, then feed speed measurements of 18 m/s.
  const IhsMeasurement seed = PosMeas(kLat0, kLon0, 5.0, SecToNs(0.0));
  ops.update(inst, &seed);
  for (int i = 1; i <= 20; ++i) {
    const IhsMeasurement s = ScalarMeas(IHS_MEAS_SPEED, 18.0, 0.25, SecToNs(i));
    ops.update(inst, &s);
  }
  IhsPosition out{};
  ops.estimate(inst, SecToNs(20), &out);
  std::printf("speed    : reported=%.2f m/s (measured 18.0)\n", out.speed_mps);
  Check(std::abs(out.speed_mps - 18.0) < 2.0, "speed measurement converges v");
  ops.destroy(inst);
}

// A yaw-rate measurement (gyro/CAN) sets omega, so a coasted estimate curves.
// Seed a straight eastward track (bearing 90 deg), inject a yaw rate, then
// coast and confirm the heading rotated.
void TestYawRateCurvesPrediction() {
  const IhsLocationFilterOps& ops = KalmanCtrvFilterOps();
  void* inst = ops.create(nullptr, nullptr);
  const auto truth =
      ihs::location::test::StraightTrack(kLat0, kLon0, 12.0, 0.0, 20, 0.5);
  for (const auto& s : truth) {
    const IhsMeasurement m = PosMeas(s.lat, s.lon, 3.0, s.t_ns);
    ops.update(inst, &m);
  }
  const uint64_t t = truth.back().t_ns;
  IhsPosition before{};
  ops.estimate(inst, t, &before);
  // Strong left yaw for a few samples (tight variance so omega moves).
  for (int i = 1; i <= 5; ++i) {
    const IhsMeasurement y =
        ScalarMeas(IHS_MEAS_YAW_RATE, 0.5, 0.0025, t + SecToNs(i * 0.1));
    ops.update(inst, &y);
  }
  IhsPosition after{};
  ops.estimate(inst, t + SecToNs(1.5), &after);
  std::printf("yaw-rate : bearing %.1f deg -> %.1f deg after a coasted turn\n",
              before.bearing_deg, after.bearing_deg);
  Check(before.has_bearing != 0 && after.has_bearing != 0,
        "bearing valid before and after");
  Check(std::abs(after.bearing_deg - before.bearing_deg) > 10.0,
        "a yaw-rate measurement curves the coasted heading");
  ops.destroy(inst);
}

// A single gross position spike must be gated out by the 2-DOF NIS check.
void TestOutlierRejection() {
  const IhsLocationFilterOps& ops = KalmanCtrvFilterOps();
  void* inst = ops.create(nullptr, nullptr);
  // Settle on a stationary point.
  for (int i = 0; i < 20; ++i) {
    const IhsMeasurement m = PosMeas(kLat0, kLon0, 5.0, SecToNs(i));
    ops.update(inst, &m);
  }
  IhsPosition before{};
  ops.estimate(inst, SecToNs(20), &before);
  // One gross outlier ~500 m north.
  const double bad_lat = kLat0 + 500.0 / ihs::location::test::MetersPerDegLat();
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

void TestCoastingGrowsSigma() {
  const IhsLocationFilterOps& ops = KalmanCtrvFilterOps();
  void* inst = ops.create(nullptr, nullptr);
  const auto truth =
      ihs::location::test::StraightTrack(kLat0, kLon0, 6.0, 0.0, 20, 1.0);
  for (const auto& s : truth) {
    const IhsMeasurement m = PosMeas(s.lat, s.lon, 5.0, s.t_ns);
    ops.update(inst, &m);
  }
  const uint64_t t = truth.back().t_ns;
  IhsPosition a{};
  IhsPosition b{};
  ops.estimate(inst, t + SecToNs(1.0), &a);
  ops.estimate(inst, t + SecToNs(10.0), &b);
  std::printf("coast    : sigma_e %.2f m -> %.2f m over +9 s\n", a.sigma_e_m,
              b.sigma_e_m);
  Check(b.sigma_e_m > a.sigma_e_m && b.sigma_n_m > a.sigma_n_m,
        "reported accuracy degrades while coasting");
  ops.destroy(inst);
}

}  // namespace

int main() {
  TestJacobianNumeric();
  TestSeeding();
  TestBeatsRawStraight();
  TestBeatsCvOnTurn();
  TestSpeedUpdate();
  TestYawRateCurvesPrediction();
  TestOutlierRejection();
  TestCoastingGrowsSigma();

  if (g_failures == 0) {
    std::printf("kalman_ctrv_test: all %d checks passed\n", g_tests);
  } else {
    std::fprintf(stderr, "kalman_ctrv_test: %d/%d checks FAILED\n", g_failures,
                 g_tests);
  }
  return g_failures == 0 ? 0 : 1;
}
