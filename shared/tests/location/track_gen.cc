/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "track_gen.hpp"

#include <cmath>

namespace ihs::location::test {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kMetersPerDegLat = 111320.0;
uint64_t SecToNs(double s) {
  // Guard the cast: a negative double to uint64_t is undefined behavior.
  // Callers pass t >= 0, but keep the conversion total.
  return s <= 0.0 ? 0 : static_cast<uint64_t>(s * 1e9);
}
}  // namespace

double MetersPerDegLat() {
  return kMetersPerDegLat;
}
double MetersPerDegLon(double lat_deg) {
  return kMetersPerDegLat * std::cos(lat_deg * kPi / 180.0);
}

double MetersBetween(double lat_a,
                     double lon_a,
                     double lat_b,
                     double lon_b,
                     double ref_lat) {
  const double de = (lon_a - lon_b) * MetersPerDegLon(ref_lat);
  const double dn = (lat_a - lat_b) * MetersPerDegLat();
  return std::sqrt(de * de + dn * dn);
}

uint64_t Rng::Next() {
  // splitmix64.
  state_ += 0x9E3779B97F4A7C15ULL;
  uint64_t z = state_;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

double Rng::Uniform() {
  // Top 53 bits → [0, 1).
  return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0);
}

double Rng::Gaussian() {
  // Box-Muller. u1 in (0, 1] to avoid log(0).
  double u1 = Uniform();
  if (u1 <= 0.0) {
    u1 = 1.0 / 9007199254740992.0;
  }
  const double u2 = Uniform();
  return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
}

// Convert an east/north offset in meters (from the origin) to lat/lon.
static void OffsetToLatLon(double lat0,
                           double lon0,
                           double east_m,
                           double north_m,
                           double& lat,
                           double& lon) {
  lat = lat0 + north_m / MetersPerDegLat();
  lon = lon0 + east_m / MetersPerDegLon(lat0);
}

std::vector<TruthSample> StraightTrack(double lat0,
                                       double lon0,
                                       double v_e,
                                       double v_n,
                                       int n,
                                       double dt_s) {
  std::vector<TruthSample> out;
  out.reserve(static_cast<size_t>(n < 0 ? 0 : n));
  for (int i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) * dt_s;
    TruthSample s;
    s.t_ns = SecToNs(t);
    OffsetToLatLon(lat0, lon0, v_e * t, v_n * t, s.lat, s.lon);
    s.v_e = v_e;
    s.v_n = v_n;
    out.push_back(s);
  }
  return out;
}

std::vector<TruthSample> ConstantTurnTrack(double lat0,
                                           double lon0,
                                           double speed_mps,
                                           double yaw_rate_dps,
                                           double heading0_deg,
                                           int n,
                                           double dt_s) {
  std::vector<TruthSample> out;
  out.reserve(static_cast<size_t>(n < 0 ? 0 : n));
  double east = 0.0;
  double north = 0.0;
  for (int i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) * dt_s;
    // Heading (clockwise from north) advancing at the yaw rate.
    const double hdg = (heading0_deg + yaw_rate_dps * t) * kPi / 180.0;
    const double v_e = speed_mps * std::sin(hdg);  // east = sin(heading)
    const double v_n = speed_mps * std::cos(hdg);  // north = cos(heading)
    TruthSample s;
    s.t_ns = SecToNs(t);
    OffsetToLatLon(lat0, lon0, east, north, s.lat, s.lon);
    s.v_e = v_e;
    s.v_n = v_n;
    out.push_back(s);
    // Integrate position for the next sample (forward Euler; dt is small).
    east += v_e * dt_s;
    north += v_n * dt_s;
  }
  return out;
}

std::vector<TruthSample> StationaryTrack(double lat0,
                                         double lon0,
                                         int n,
                                         double dt_s) {
  return StraightTrack(lat0, lon0, 0.0, 0.0, n, dt_s);
}

std::vector<NoisyFix> AddNoise(const std::vector<TruthSample>& truth,
                               double sigma_m,
                               Rng& rng) {
  std::vector<NoisyFix> out;
  out.reserve(truth.size());
  for (const TruthSample& s : truth) {
    const double de = rng.Gaussian() * sigma_m;  // east error, m
    const double dn = rng.Gaussian() * sigma_m;  // north error, m
    NoisyFix f;
    f.t_ns = s.t_ns;
    f.lat = s.lat + dn / MetersPerDegLat();
    f.lon = s.lon + de / MetersPerDegLon(s.lat);
    f.sigma_m = sigma_m;
    out.push_back(f);
  }
  return out;
}

}  // namespace ihs::location::test
