/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef IHS_LOC_KALMAN_CTRV_H_
#define IHS_LOC_KALMAN_CTRV_H_

#include "ihs/location.h"

namespace ihs::location {

// The constant-turn-rate-and-velocity (CTRV) extended Kalman filter, registered
// under "kalman.ctrv".
//
// State x = [px, py, psi, v, omega]: position east/north (meters) in a local
// tangent plane anchored at the first fix, heading psi (rad, CCW from east),
// speed v (m/s), and yaw rate omega (rad/s). Unlike kalman.cv's straight-line
// model, CTRV follows a constant-radius arc, so it tracks a turning vehicle
// without lagging the corner — the right model for a car. The motion model is
// nonlinear, so predict uses its analytic Jacobian (an EKF); the measurement
// updates are all linear.
//
// It corrects from GNSS position (IHS_MEAS_POSITION_LLA), and — the reason the
// model earns its keep — directly from a yaw-rate (IHS_MEAS_YAW_RATE) or ground
// speed (IHS_MEAS_SPEED) measurement when a gyro/CAN source provides one, each
// a scalar linear update on omega / v. estimate(t) predicts the state forward
// to
// @t, so a UI-rate poll gets a fresh position that curves correctly between
// fixes.
//
// The same robustness as kalman.cv: a per-measurement chi-square gate rejects
// outliers and a persistent run resets to the raw fix; CLOCK_MONOTONIC drives
// dt; out-of-order measurements are dropped; the tangent plane re-anchors past
// ~10 km. A 5x5 fixed-size matrix implementation, no Eigen.
//
// config (optional, may be NULL/empty): "qa=<value>" sets the longitudinal
// acceleration noise density and "qw=<value>" the yaw-acceleration noise
// (space- or comma-separated), the two manoeuvring tuning knobs.

// Register the filter under "kalman.ctrv" in the process-wide location
// registry. Idempotent (re-registration replaces the entry). Returns true on
// success.
bool RegisterKalmanCtrvFilter();

// The filter's ops table, exposed so it can be unit-tested directly (create /
// update / estimate / destroy) without going through the registry or a Manager.
const IhsLocationFilterOps& KalmanCtrvFilterOps();

// Test hooks over the CTRV motion model: @x is the 5-state [px, py, psi, v,
// omega]. Predict fills @out5 with the state propagated by @dt; Jacobian fills
// @out25 (row-major 5x5) with the analytic d f / d x. Exposed so a unit test
// can finite-difference the predict and compare to the analytic Jacobian — an
// EKF's Jacobian is its most error-prone part, and a silent sign/index slip
// there is exactly what a behavioral test on a gentle track can miss.
void KalmanCtrvPredictForTest(const double x[5], double dt, double out5[5]);
void KalmanCtrvJacobianForTest(const double x[5], double dt, double out25[25]);

}  // namespace ihs::location

#endif  // IHS_LOC_KALMAN_CTRV_H_
