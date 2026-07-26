/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "kalman_ctrv.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "location.hpp"  // ValidLatLon

namespace ihs::location {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMetersPerDegLat = 111320.0;

// Below this yaw rate the arc is straight; use the limit form to avoid a
// 1/omega singularity.
constexpr double kOmegaEps = 1.0e-4;  // rad/s

// Process-noise 1-sigma (the two tuning knobs): longitudinal acceleration and
// yaw acceleration. The "qa="/"qw=" config values are these sigmas, squared to
// variances in Create(). Defaults suit a road vehicle.
constexpr double kDefaultSigmaA = 2.0;   // m/s^2
constexpr double kDefaultSigmaW = 0.15;  // rad/s^2
// Seed variances for the states a single position cannot observe.
constexpr double kInitHeadingVar = kPi * kPi;   // rad^2 (heading unknown)
constexpr double kInitSpeedVar = 50.0 * 50.0;   // (m/s)^2
constexpr double kInitYawRateVar = 1.0 * 1.0;   // (rad/s)^2
constexpr double kDefaultPosVar = 10.0 * 10.0;  // m^2
// Chi-square gates: 2-DOF (position) and 1-DOF (scalar) at p=0.001.
constexpr double kChi2Gate2Dof = 13.816;
constexpr double kChi2Gate1Dof = 10.828;
constexpr int kMaxConsecutiveRejects = 3;
constexpr double kReanchorMeters = 10000.0;
constexpr double kMinSpeedForBearing = 0.5;

double MetersPerDegLon(double lat_deg) {
  // Clamp near the poles (cos -> 0) so the meters<->degrees divisor stays
  // finite.
  constexpr double kMinMetersPerDegLon = 1.0;
  const double scale = kMetersPerDegLat * std::cos(lat_deg * kPi / 180.0);
  return scale > kMinMetersPerDegLon ? scale : kMinMetersPerDegLon;
}

double FiniteVarianceOr(double variance, double fallback) {
  return (std::isfinite(variance) && variance >= 0.0) ? variance : fallback;
}

using Vec5 = std::array<double, 5>;
using Mat5 = std::array<std::array<double, 5>, 5>;

Mat5 Zero5() {
  return Mat5{};
}

Mat5 Identity5() {
  Mat5 m = Zero5();
  for (std::size_t i = 0; i < 5; ++i) {
    m[i][i] = 1.0;
  }
  return m;
}

Mat5 Mul(const Mat5& a, const Mat5& b) {
  Mat5 c = Zero5();
  for (std::size_t i = 0; i < 5; ++i) {
    for (std::size_t k = 0; k < 5; ++k) {
      const double aik = a[i][k];
      for (std::size_t j = 0; j < 5; ++j) {
        c[i][j] += aik * b[k][j];
      }
    }
  }
  return c;
}

Mat5 Transpose(const Mat5& a) {
  Mat5 t = Zero5();
  for (std::size_t i = 0; i < 5; ++i) {
    for (std::size_t j = 0; j < 5; ++j) {
      t[i][j] = a[j][i];
    }
  }
  return t;
}

// Parse "qa=<v>" / "qw=<v>" out of the config (space/comma separated, any
// order).
double ParseNamed(const char* config, const char* key, double fallback) {
  if (config == nullptr) {
    return fallback;
  }
  const char* at = std::strstr(config, key);
  if (at == nullptr) {
    return fallback;
  }
  char* end = nullptr;
  const double v = std::strtod(at + std::strlen(key), &end);
  return (end != at + std::strlen(key) && std::isfinite(v) && v > 0.0)
             ? v
             : fallback;
}

// Constant-turn-rate-and-velocity extended Kalman filter.
class KalmanCtrv {
 public:
  // @qa_var / @qw_var are the acceleration / yaw-acceleration noise variances
  // (1-sigma^2) used by the process-noise model.
  KalmanCtrv(double qa_var, double qw_var) : qa_(qa_var), qw_(qw_var) {}

  void Update(const IhsMeasurement& m) {
    switch (m.kind) {
      case IHS_MEAS_POSITION_LLA:
        UpdatePosition(m);
        break;
      case IHS_MEAS_YAW_RATE:
        if (m.value_count >= 1) {
          UpdateScalar(m, /*state=*/4, FiniteVarianceOr(m.variance[0], 0.01));
        }
        break;
      case IHS_MEAS_SPEED:
        if (m.value_count >= 1) {
          UpdateScalar(m, /*state=*/3, FiniteVarianceOr(m.variance[0], 1.0));
        }
        break;
      default:
        break;  // other kinds are not CTRV inputs
    }
  }

  int Estimate(uint64_t t_monotonic_ns, IhsPosition& out) const {
    if (!have_state_) {
      return 0;
    }
    double dt = SecondsSince(t_monotonic_ns);
    if (dt < 0.0) {
      dt = 0.0;
    }
    Vec5 x = x_;
    Mat5 p = p_;
    PredictInto(x_, p_, dt, x, p);

    out.latitude = anchor_lat_ + x[1] / kMetersPerDegLat;
    out.longitude = anchor_lon_ + x[0] / MetersPerDegLon(anchor_lat_);
    out.mode = last_mode_;
    const double v = x[3];
    out.speed_mps = std::fabs(v);
    const double ve = v * std::cos(x[2]);
    const double vn = v * std::sin(x[2]);
    if (out.speed_mps > kMinSpeedForBearing) {
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
    out.sigma_e_m = std::sqrt(p[0][0] > 0.0 ? p[0][0] : 0.0);
    out.sigma_n_m = std::sqrt(p[1][1] > 0.0 ? p[1][1] : 0.0);
    out.sigma_v_mps = std::sqrt(p[3][3] > 0.0 ? p[3][3] : 0.0);
    return 1;
  }

  // Exposed for the numerical-Jacobian unit test.
  static Vec5 PredictStateFor(const Vec5& x, double dt) {
    return PredictState(x, dt);
  }
  static Mat5 JacobianFor(const Vec5& x, double dt) { return Jacobian(x, dt); }

 private:
  [[nodiscard]] double SecondsSince(uint64_t t_monotonic_ns) const {
    const int64_t d =
        static_cast<int64_t>(t_monotonic_ns) - static_cast<int64_t>(last_t_ns_);
    return static_cast<double>(d) * 1e-9;
  }

  // CTRV motion model: propagate the state forward by dt.
  static Vec5 PredictState(const Vec5& x, double dt) {
    const double px = x[0];
    const double py = x[1];
    const double psi = x[2];
    const double v = x[3];
    const double w = x[4];
    Vec5 out = x;
    if (std::fabs(w) > kOmegaEps) {
      const double psi_new = psi + w * dt;
      out[0] = px + (v / w) * (std::sin(psi_new) - std::sin(psi));
      out[1] = py + (v / w) * (-std::cos(psi_new) + std::cos(psi));
      out[2] = psi_new;
    } else {
      out[0] = px + v * std::cos(psi) * dt;
      out[1] = py + v * std::sin(psi) * dt;
      out[2] = psi + w * dt;
    }
    out[3] = v;
    out[4] = w;
    return out;
  }

  // Analytic Jacobian d f / d x of PredictState (verified numerically in
  // tests).
  static Mat5 Jacobian(const Vec5& x, double dt) {
    const double psi = x[2];
    const double v = x[3];
    const double w = x[4];
    Mat5 f = Identity5();
    if (std::fabs(w) > kOmegaEps) {
      const double a = psi + w * dt;
      const double sa = std::sin(a);
      const double ca = std::cos(a);
      const double sp = std::sin(psi);
      const double cp = std::cos(psi);
      f[0][2] = (v / w) * (ca - cp);
      f[0][3] = (1.0 / w) * (sa - sp);
      f[0][4] = (v * dt / w) * ca - (v / (w * w)) * (sa - sp);
      f[1][2] = (v / w) * (sa - sp);
      f[1][3] = (1.0 / w) * (-ca + cp);
      f[1][4] = (v * dt / w) * sa - (v / (w * w)) * (-ca + cp);
    } else {
      // Straight-line branch: PredictState uses px += v cos(psi) dt (no omega
      // term), so d(px,py)/d(omega) is exactly 0 here — the Jacobian must match
      // the predict actually used, not the w->0 limit of the arc form, or the
      // covariance is inconsistent with the propagation. Omega stays observable
      // across steps through psi' = psi + omega dt (f[2][4] below).
      const double sp = std::sin(psi);
      const double cp = std::cos(psi);
      f[0][2] = -v * sp * dt;
      f[0][3] = cp * dt;
      f[1][2] = v * cp * dt;
      f[1][3] = sp * dt;
    }
    f[2][4] = dt;
    return f;
  }

  // Process noise Q(dt) = G diag(qa, qw) G^T from acceleration /
  // yaw-acceleration.
  [[nodiscard]] Mat5 ProcessNoise(double dt, double psi) const {
    const double h = 0.5 * dt * dt;
    // G columns: [accel, yaw-accel].
    const double g0[5] = {h * std::cos(psi), h * std::sin(psi), 0.0, dt, 0.0};
    const double g1[5] = {0.0, 0.0, h, 0.0, dt};
    Mat5 q = Zero5();
    for (std::size_t i = 0; i < 5; ++i) {
      for (std::size_t j = 0; j < 5; ++j) {
        q[i][j] = qa_ * g0[i] * g0[j] + qw_ * g1[i] * g1[j];
      }
    }
    return q;
  }

  // Predict state and covariance forward by dt into @x_out, @p_out (may alias
  // the inputs). P' = Fj P Fj^T + Q.
  void PredictInto(const Vec5& x_in,
                   const Mat5& p_in,
                   double dt,
                   Vec5& x_out,
                   Mat5& p_out) const {
    const Mat5 f = Jacobian(x_in, dt);
    const Mat5 q = ProcessNoise(dt, x_in[2]);
    const Vec5 xn = PredictState(x_in, dt);
    const Mat5 fp = Mul(f, p_in);
    Mat5 pn = Mul(fp, Transpose(f));
    for (std::size_t i = 0; i < 5; ++i) {
      for (std::size_t j = 0; j < 5; ++j) {
        pn[i][j] += q[i][j];
      }
    }
    x_out = xn;
    p_out = pn;
  }

  void Predict(double dt) { PredictInto(x_, p_, dt, x_, p_); }

  void Seed(double lat, double lon, double var_e, double var_n, uint64_t t_ns) {
    anchor_lat_ = lat;
    anchor_lon_ = lon;
    x_ = {0.0, 0.0, 0.0, 0.0, 0.0};
    p_ = Zero5();
    p_[0][0] = var_e;
    p_[1][1] = var_n;
    p_[2][2] = kInitHeadingVar;
    p_[3][3] = kInitSpeedVar;
    p_[4][4] = kInitYawRateVar;
    last_t_ns_ = t_ns;
    have_state_ = true;
    consecutive_rejects_ = 0;
  }

  void UpdatePosition(const IhsMeasurement& m) {
    if (m.value_count < 2 || !ValidLatLon(m.value[0], m.value[1])) {
      return;
    }
    const double lat = m.value[0];
    const double lon = m.value[1];
    // Service convention: variance[0] = east, variance[1] = north.
    const double var_e = FiniteVarianceOr(m.variance[0], kDefaultPosVar);
    const double var_n = FiniteVarianceOr(m.variance[1], kDefaultPosVar);
    // mode describes the last ACCEPTED fix, so it is set only on seed/reset and
    // on acceptance below — never for a rejected or out-of-order measurement.
    const int mode = m.value_count >= 3 ? 3 : 2;

    if (!have_state_) {
      Seed(lat, lon, var_e, var_n, m.t_monotonic_ns);
      last_mode_ = mode;
      return;
    }
    const double dt = SecondsSince(m.t_monotonic_ns);
    if (dt < 0.0) {
      return;  // out of order: drop
    }
    Predict(dt);
    last_t_ns_ = m.t_monotonic_ns;

    const double z_e = (lon - anchor_lon_) * MetersPerDegLon(anchor_lat_);
    const double z_n = (lat - anchor_lat_) * kMetersPerDegLat;
    const double y_e = z_e - x_[0];
    const double y_n = z_n - x_[1];
    // S = H P H^T + R (top-left 2x2 of P plus R), H = [I2 | 0].
    const double s00 = p_[0][0] + var_e;
    const double s01 = p_[0][1];
    const double s10 = p_[1][0];
    const double s11 = p_[1][1] + var_n;
    const double det = s00 * s11 - s01 * s10;
    if (!(std::fabs(det) > 0.0)) {
      return;
    }
    const double inv = 1.0 / det;
    const double si00 = s11 * inv;
    const double si01 = -s01 * inv;
    const double si10 = -s10 * inv;
    const double si11 = s00 * inv;
    const double nis =
        y_e * (si00 * y_e + si01 * y_n) + y_n * (si10 * y_e + si11 * y_n);
    if (nis > kChi2Gate2Dof) {
      if (++consecutive_rejects_ >= kMaxConsecutiveRejects) {
        Seed(lat, lon, var_e, var_n, m.t_monotonic_ns);
        last_mode_ = mode;  // reset adopts the measurement
      }
      return;
    }
    consecutive_rejects_ = 0;
    last_mode_ = mode;  // measurement accepted

    // K = P H^T S^-1 (P H^T is the first two columns of P).
    std::array<std::array<double, 2>, 5> k{};
    for (std::size_t i = 0; i < 5; ++i) {
      const double a = p_[i][0];
      const double b = p_[i][1];
      k[i][0] = a * si00 + b * si10;
      k[i][1] = a * si01 + b * si11;
    }
    for (std::size_t i = 0; i < 5; ++i) {
      x_[i] += k[i][0] * y_e + k[i][1] * y_n;
    }
    // P = (I - K H) P, H picks rows 0,1.
    Mat5 ikh = Identity5();
    for (std::size_t i = 0; i < 5; ++i) {
      ikh[i][0] -= k[i][0];
      ikh[i][1] -= k[i][1];
    }
    p_ = Mul(ikh, p_);
    Symmetrize();
    ReanchorIfFar();
  }

  // Linear scalar update on a single @state index (yaw rate = 4, speed = 3).
  void UpdateScalar(const IhsMeasurement& m, std::size_t state, double r) {
    if (!have_state_) {
      return;  // no position anchor yet; a scalar alone cannot seed a fix
    }
    const double dt = SecondsSince(m.t_monotonic_ns);
    if (dt < 0.0) {
      return;
    }
    Predict(dt);
    last_t_ns_ = m.t_monotonic_ns;

    const double y = m.value[0] - x_[state];
    const double s = p_[state][state] + r;
    if (!(s > 0.0)) {
      return;
    }
    const double nis = y * y / s;
    if (nis > kChi2Gate1Dof) {
      return;  // reject a scalar outlier (no reset: position drives the fix)
    }
    // K = P H^T / S = column `state` of P over S.
    Vec5 kgain{};
    for (std::size_t i = 0; i < 5; ++i) {
      kgain[i] = p_[i][state] / s;
    }
    for (std::size_t i = 0; i < 5; ++i) {
      x_[i] += kgain[i] * y;
    }
    // P = (I - K H) P, H = e_state^T.
    Mat5 ikh = Identity5();
    for (std::size_t i = 0; i < 5; ++i) {
      ikh[i][state] -= kgain[i];
    }
    p_ = Mul(ikh, p_);
    Symmetrize();
  }

  void Symmetrize() {
    for (std::size_t i = 0; i < 5; ++i) {
      for (std::size_t j = i + 1; j < 5; ++j) {
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
    const double old_anchor_lat = anchor_lat_;
    anchor_lat_ += x_[1] / kMetersPerDegLat;
    anchor_lon_ += x_[0] / MetersPerDegLon(old_anchor_lat);
    x_[0] = 0.0;
    x_[1] = 0.0;
  }

  double qa_;
  double qw_;
  Vec5 x_{{0.0, 0.0, 0.0, 0.0, 0.0}};
  Mat5 p_ = Zero5();
  double anchor_lat_ = 0.0;
  double anchor_lon_ = 0.0;
  uint64_t last_t_ns_ = 0;
  int last_mode_ = 2;
  bool have_state_ = false;
  int consecutive_rejects_ = 0;
};

void* Create(void* /*user_data*/, const char* config) {
  try {
    // The config knobs are 1-sigma accelerations; square them to the variances
    // the process-noise model uses, so "qa=2" means 2 m/s^2 (the default).
    const double sigma_a = ParseNamed(config, "qa=", kDefaultSigmaA);
    const double sigma_w = ParseNamed(config, "qw=", kDefaultSigmaW);
    return new KalmanCtrv(sigma_a * sigma_a, sigma_w * sigma_w);
  } catch (...) {
    return nullptr;
  }
}

void Destroy(void* instance) {
  delete static_cast<KalmanCtrv*>(instance);
}

void Update(void* instance, const IhsMeasurement* m) {
  if (instance != nullptr && m != nullptr) {
    static_cast<KalmanCtrv*>(instance)->Update(*m);
  }
}

int Estimate(void* instance, uint64_t t_monotonic_ns, IhsPosition* out) {
  if (instance == nullptr || out == nullptr) {
    return 0;
  }
  return static_cast<const KalmanCtrv*>(instance)->Estimate(t_monotonic_ns,
                                                            *out);
}

}  // namespace

const IhsLocationFilterOps& KalmanCtrvFilterOps() {
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

bool RegisterKalmanCtrvFilter() {
  return ihs_location_register_filter("kalman.ctrv", &KalmanCtrvFilterOps(),
                                      nullptr) == 1;
}

void KalmanCtrvPredictForTest(const double x[5], double dt, double out5[5]) {
  Vec5 xin;
  for (std::size_t i = 0; i < 5; ++i) {
    xin[i] = x[i];
  }
  const Vec5 xout = KalmanCtrv::PredictStateFor(xin, dt);
  for (std::size_t i = 0; i < 5; ++i) {
    out5[i] = xout[i];
  }
}

void KalmanCtrvJacobianForTest(const double x[5], double dt, double out25[25]) {
  Vec5 xin;
  for (std::size_t i = 0; i < 5; ++i) {
    xin[i] = x[i];
  }
  const Mat5 f = KalmanCtrv::JacobianFor(xin, dt);
  for (std::size_t i = 0; i < 5; ++i) {
    for (std::size_t j = 0; j < 5; ++j) {
      out25[i * 5 + j] = f[i][j];
    }
  }
}

}  // namespace ihs::location
