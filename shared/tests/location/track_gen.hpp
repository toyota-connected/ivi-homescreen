/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef IHS_LOC_TEST_TRACK_GEN_H_
#define IHS_LOC_TEST_TRACK_GEN_H_

#include <cstdint>
#include <vector>

// Synthetic ground truth and measurement noise for the location filter harness.
// Deterministic (a splitmix64 RNG + Box-Muller, so the sequence is identical on
// libstdc++ and libc++ — unlike std::normal_distribution), so CI is stable.
//
// Everything is planar: tracks are generated in a local east/north tangent
// plane (meters) around a fixed origin and converted to lat/lon, and errors are
// measured back in meters. Over the short tracks the tests use, the flat-earth
// approximation is well below the noise being studied.
namespace ihs::location::test {

// One ground-truth sample: true position and true velocity at arrival time t.
struct TruthSample {
  uint64_t t_ns = 0;
  double lat = 0.0;  // degrees
  double lon = 0.0;  // degrees
  double v_e = 0.0;  // true east velocity, m/s
  double v_n = 0.0;  // true north velocity, m/s
};

// A noisy position measurement derived from a truth sample.
struct NoisyFix {
  uint64_t t_ns = 0;
  double lat = 0.0;      // degrees, truth + Gaussian error
  double lon = 0.0;      // degrees
  double sigma_m = 0.0;  // the 1-sigma the noise was drawn at (per axis), m
};

// Local flat-earth scale, meters per degree. A degree of latitude is very
// nearly constant, so MetersPerDegLat() takes no argument; a degree of
// longitude shrinks with latitude (by cos), so MetersPerDegLon() takes it.
double MetersPerDegLat();
double MetersPerDegLon(double lat_deg);

// Great-circle-free planar distance between two lat/lon points, in meters,
// using the local scale at @ref_lat. Adequate for the short synthetic tracks.
double MetersBetween(double lat_a,
                     double lon_a,
                     double lat_b,
                     double lon_b,
                     double ref_lat);

// Deterministic RNG: splitmix64 for uniforms, Box-Muller for a standard normal.
class Rng {
 public:
  explicit Rng(uint64_t seed) : state_(seed) {}
  double Uniform();   // [0, 1)
  double Gaussian();  // mean 0, sigma 1

 private:
  uint64_t Next();
  uint64_t state_;
};

// --- tracks (each starts at the origin at t=0, samples every @dt_s) ---------

// Constant-velocity straight line at (@v_e, @v_n) m/s.
std::vector<TruthSample> StraightTrack(double lat0,
                                       double lon0,
                                       double v_e,
                                       double v_n,
                                       int n,
                                       double dt_s);

// Constant-speed circular arc: speed @speed_mps, turn rate @yaw_rate_dps
// (deg/s), initial heading @heading0_deg (clockwise from north).
std::vector<TruthSample> ConstantTurnTrack(double lat0,
                                           double lon0,
                                           double speed_mps,
                                           double yaw_rate_dps,
                                           double heading0_deg,
                                           int n,
                                           double dt_s);

// Stationary: no motion (velocity 0). The baseline for the variance-collapse
// check.
std::vector<TruthSample> StationaryTrack(double lat0,
                                         double lon0,
                                         int n,
                                         double dt_s);

// Add per-axis Gaussian noise at @sigma_m meters to each truth sample.
std::vector<NoisyFix> AddNoise(const std::vector<TruthSample>& truth,
                               double sigma_m,
                               Rng& rng);

}  // namespace ihs::location::test

#endif  // IHS_LOC_TEST_TRACK_GEN_H_
