/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "kalman_cv.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace ihs::location {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMetersPerDegLat = 111320.0;

// Default process-noise spectral density (m^2/s^3): the manoeuvring a road
// vehicle does between fixes. Overridable via the "q=<value>" config.
constexpr double kDefaultQ = 1.0;
// Seed variance for the (unknown) initial velocity, (m/s)^2.
constexpr double kInitVelVar = 50.0 * 50.0;
// Position variance used when a measurement does not report one, m^2.
constexpr double kDefaultPosVar = 10.0 * 10.0;
// 2-DOF chi-square gate at p=0.001: a single innovation past this is treated as
// an outlier. A persistent run of them means the filter has diverged.
constexpr double kChi2Gate2Dof = 13.816;
constexpr int kMaxConsecutiveRejects = 3;
// Re-anchor the tangent plane once the state drifts this far, keeping the
// flat-earth conversion honest.
constexpr double kReanchorMeters = 10000.0;
// Report a course only above this speed; below it, bearing is noise.
constexpr double kMinSpeedForBearing = 0.5;

double MetersPerDegLon(double lat_deg) {
  return kMetersPerDegLat * std::cos(lat_deg * kPi / 180.0);
}

// A measurement variance is usable only if finite and non-negative; a negative
// value is the "unknown" sentinel and a non-finite one (NaN/+Inf) is garbage
// that would poison S / NIS / K. Either falls back to @fallback.
double FiniteVarianceOr(double variance, double fallback) {
  return (std::isfinite(variance) && variance >= 0.0) ? variance : fallback;
}

using Mat4 = std::array<std::array<double, 4>, 4>;

Mat4 Zero4() {
  return Mat4{};
}

Mat4 Mul(const Mat4& a, const Mat4& b) {
  Mat4 c = Zero4();
  for (std::size_t i = 0; i < 4; ++i) {
    for (std::size_t k = 0; k < 4; ++k) {
      const double aik = a[i][k];
      for (std::size_t j = 0; j < 4; ++j) {
        c[i][j] += aik * b[k][j];
      }
    }
  }
  return c;
}

Mat4 Transpose(const Mat4& a) {
  Mat4 t = Zero4();
  for (std::size_t i = 0; i < 4; ++i) {
    for (std::size_t j = 0; j < 4; ++j) {
      t[i][j] = a[j][i];
    }
  }
  return t;
}

double ParseQ(const char* config) {
  if (config == nullptr) {
    return kDefaultQ;
  }
  const char* at = std::strstr(config, "q=");
  if (at == nullptr) {
    return kDefaultQ;
  }
  char* end = nullptr;
  const double v = std::strtod(at + 2, &end);
  return (end != at + 2 && std::isfinite(v) && v > 0.0) ? v : kDefaultQ;
}

// Constant-velocity Kalman filter in a local east/north tangent plane.
class KalmanCv {
 public:
  explicit KalmanCv(double q) : q_(q) {}

  void Update(const IhsMeasurement& m) {
    if (m.kind != IHS_MEAS_POSITION_LLA || m.value_count < 2) {
      return;  // position-only filter; other kinds are future inputs
    }
    const double lat = m.value[0];
    const double lon = m.value[1];
    // value[] is [lat, lon] (lat = north, lon = east), but variance[] follows
    // the service convention variance[0] = east, variance[1] = north — the same
    // order as IhsPosition's sigma_e_m/sigma_n_m and the Manager passthrough —
    // decoupled from the value order. A non-finite or negative variance is
    // "unknown": +Inf would otherwise make S infinite and produce NaNs through
    // S^-1 / NIS / K that corrupt the state.
    const double var_e = FiniteVarianceOr(m.variance[0], kDefaultPosVar);
    const double var_n = FiniteVarianceOr(m.variance[1], kDefaultPosVar);
    const int mode = m.value_count >= 3 ? 3 : 2;

    if (!have_state_) {
      Seed(lat, lon, var_e, var_n, m.t_monotonic_ns, mode);
      return;
    }

    const double e_meas = (lon - anchor_lon_) * MetersPerDegLon(anchor_lat_);
    const double n_meas = (lat - anchor_lat_) * kMetersPerDegLat;
    const double dt = SecondsSince(m.t_monotonic_ns);
    if (dt < 0.0) {
      return;  // older than the last correction: drop (replay buffer is the
               // upgrade if this proves common)
    }

    Predict(dt);

    // Innovation and its covariance S = H P H^T + R (the top-left 2x2 of P,
    // since H selects the position states).
    const double y_e = e_meas - x_[0];
    const double y_n = n_meas - x_[1];
    const double s00 = p_[0][0] + var_e;
    const double s01 = p_[0][1];
    const double s10 = p_[1][0];
    const double s11 = p_[1][1] + var_n;
    const double det = s00 * s11 - s01 * s10;
    last_t_ns_ = m.t_monotonic_ns;
    last_mode_ = mode;
    if (!(std::fabs(det) > 0.0)) {
      return;  // singular S (degenerate covariance): skip the correction
    }
    const double inv = 1.0 / det;
    const double si00 = s11 * inv;
    const double si01 = -s01 * inv;
    const double si10 = -s10 * inv;
    const double si11 = s00 * inv;

    // Normalized innovation squared; gate outliers, reset on persistent
    // divergence.
    const double nis =
        y_e * (si00 * y_e + si01 * y_n) + y_n * (si10 * y_e + si11 * y_n);
    if (nis > kChi2Gate2Dof) {
      if (++consecutive_rejects_ >= kMaxConsecutiveRejects) {
        Seed(lat, lon, var_e, var_n, m.t_monotonic_ns, mode);
      }
      return;  // keep the predicted (coasted) state; skip this correction
    }
    consecutive_rejects_ = 0;

    // Kalman gain K = P H^T S^-1 (P H^T is the first two columns of P).
    std::array<std::array<double, 2>, 4> k{};
    for (std::size_t i = 0; i < 4; ++i) {
      const double a = p_[i][0];
      const double b = p_[i][1];
      k[i][0] = a * si00 + b * si10;
      k[i][1] = a * si01 + b * si11;
    }
    for (std::size_t i = 0; i < 4; ++i) {
      x_[i] += k[i][0] * y_e + k[i][1] * y_n;
    }
    // P = (I - K H) P, then symmetrize to keep it numerically well-formed.
    Mat4 m_ikh = Zero4();
    for (std::size_t i = 0; i < 4; ++i) {
      for (std::size_t j = 0; j < 4; ++j) {
        m_ikh[i][j] = (i == j ? 1.0 : 0.0);
      }
      m_ikh[i][0] -= k[i][0];
      m_ikh[i][1] -= k[i][1];
    }
    p_ = Mul(m_ikh, p_);
    Symmetrize();

    ReanchorIfFar();
  }

  int Estimate(uint64_t t_monotonic_ns, IhsPosition& out) const {
    if (!have_state_) {
      return 0;
    }
    double dt = SecondsSince(t_monotonic_ns);
    if (dt < 0.0) {
      dt = 0.0;  // never predict backward
    }
    // Predict a copy forward to t; do not mutate the stored state.
    const double e = x_[0] + x_[2] * dt;
    const double n = x_[1] + x_[3] * dt;
    const double ve = x_[2];
    const double vn = x_[3];
    const Mat4 pp = PredictedP(dt);

    out.latitude = anchor_lat_ + n / kMetersPerDegLat;
    out.longitude = anchor_lon_ + e / MetersPerDegLon(anchor_lat_);
    out.mode = last_mode_;
    const double speed = std::sqrt(ve * ve + vn * vn);
    out.speed_mps = speed;
    if (speed > kMinSpeedForBearing) {
      double course = std::atan2(ve, vn) * 180.0 / kPi;  // clockwise from north
      if (course < 0.0) {
        course += 360.0;
      }
      out.bearing_deg = course;
      out.has_bearing = 1;
    } else {
      out.bearing_deg = 0.0;
      out.has_bearing = 0;
    }
    out.t_monotonic_ns = t_monotonic_ns;
    out.sigma_e_m = std::sqrt(pp[0][0] > 0.0 ? pp[0][0] : 0.0);
    out.sigma_n_m = std::sqrt(pp[1][1] > 0.0 ? pp[1][1] : 0.0);
    const double var_v = 0.5 * (pp[2][2] + pp[3][3]);
    out.sigma_v_mps = std::sqrt(var_v > 0.0 ? var_v : 0.0);
    return 1;
  }

 private:
  [[nodiscard]] double SecondsSince(uint64_t t_monotonic_ns) const {
    // Difference in integer nanoseconds first, then to seconds: both are
    // CLOCK_MONOTONIC ns (well under 2^63), so the int64 subtraction is exact
    // and signed. Converting each to double before subtracting would lose
    // sub-microsecond precision once the clock passes ~2^52 ns (~52 days up),
    // jittering dt and breaking out-of-order detection.
    const int64_t d =
        static_cast<int64_t>(t_monotonic_ns) - static_cast<int64_t>(last_t_ns_);
    return static_cast<double>(d) * 1e-9;
  }

  void Seed(double lat,
            double lon,
            double var_e,
            double var_n,
            uint64_t t_ns,
            int mode) {
    anchor_lat_ = lat;
    anchor_lon_ = lon;
    x_ = {0.0, 0.0, 0.0, 0.0};
    p_ = Zero4();
    p_[0][0] = var_e;
    p_[1][1] = var_n;
    p_[2][2] = kInitVelVar;
    p_[3][3] = kInitVelVar;
    last_t_ns_ = t_ns;
    last_mode_ = mode;
    have_state_ = true;
    consecutive_rejects_ = 0;
  }

  void Predict(double dt) {
    x_[0] += x_[2] * dt;
    x_[1] += x_[3] * dt;
    p_ = PredictedP(dt);
  }

  // F P F^T + Q(dt), the covariance predicted forward by dt.
  [[nodiscard]] Mat4 PredictedP(double dt) const {
    Mat4 f = Zero4();
    for (std::size_t i = 0; i < 4; ++i) {
      f[i][i] = 1.0;
    }
    f[0][2] = dt;
    f[1][3] = dt;
    Mat4 out = Mul(Mul(f, p_), Transpose(f));
    // Continuous white-noise-acceleration Q, coupling each position with its
    // own velocity.
    const double dt2 = dt * dt;
    const double dt3 = dt2 * dt;
    const double q = q_;
    out[0][0] += q * dt3 / 3.0;
    out[0][2] += q * dt2 / 2.0;
    out[2][0] += q * dt2 / 2.0;
    out[2][2] += q * dt;
    out[1][1] += q * dt3 / 3.0;
    out[1][3] += q * dt2 / 2.0;
    out[3][1] += q * dt2 / 2.0;
    out[3][3] += q * dt;
    return out;
  }

  void Symmetrize() {
    for (std::size_t i = 0; i < 4; ++i) {
      for (std::size_t j = i + 1; j < 4; ++j) {
        const double avg = 0.5 * (p_[i][j] + p_[j][i]);
        p_[i][j] = avg;
        p_[j][i] = avg;
      }
    }
  }

  void ReanchorIfFar() {
    if (std::sqrt(x_[0] * x_[0] + x_[1] * x_[1]) <= kReanchorMeters) {
      return;
    }
    // Fold the accumulated offset into the anchor; the covariance is in meters
    // and invariant under the translation, so only the position states reset.
    // The east offset was measured in the old frame, so convert it back with
    // the old anchor latitude's longitude scale.
    const double old_anchor_lat = anchor_lat_;
    anchor_lat_ += x_[1] / kMetersPerDegLat;
    anchor_lon_ += x_[0] / MetersPerDegLon(old_anchor_lat);
    x_[0] = 0.0;
    x_[1] = 0.0;
  }

  double q_;
  std::array<double, 4> x_{{0.0, 0.0, 0.0, 0.0}};
  Mat4 p_ = Zero4();
  double anchor_lat_ = 0.0;
  double anchor_lon_ = 0.0;
  uint64_t last_t_ns_ = 0;
  int last_mode_ = 2;
  bool have_state_ = false;
  int consecutive_rejects_ = 0;
};

void* Create(void* /*user_data*/, const char* config) {
  try {
    return new KalmanCv(ParseQ(config));
  } catch (...) {
    return nullptr;  // never let an exception cross the C ABI
  }
}

void Destroy(void* instance) {
  delete static_cast<KalmanCv*>(instance);
}

void Update(void* instance, const IhsMeasurement* m) {
  if (instance != nullptr && m != nullptr) {
    static_cast<KalmanCv*>(instance)->Update(*m);
  }
}

int Estimate(void* instance, uint64_t t_monotonic_ns, IhsPosition* out) {
  if (instance == nullptr || out == nullptr) {
    return 0;
  }
  return static_cast<const KalmanCv*>(instance)->Estimate(t_monotonic_ns, *out);
}

}  // namespace

const IhsLocationFilterOps& KalmanCvFilterOps() {
  static const IhsLocationFilterOps ops = [] {
    IhsLocationFilterOps o{};
    o.struct_size = sizeof(o);
    o.create = &Create;
    o.destroy = &Destroy;
    o.update = &Update;
    o.estimate = &Estimate;
    return o;
  }();
  return ops;
}

bool RegisterKalmanCvFilter() {
  return ihs_location_register_filter("kalman.cv", &KalmanCvFilterOps(),
                                      nullptr) == 1;
}

}  // namespace ihs::location
