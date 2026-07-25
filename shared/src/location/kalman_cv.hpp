/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef IHS_LOC_KALMAN_CV_H_
#define IHS_LOC_KALMAN_CV_H_

#include "ihs/location.h"

namespace ihs::location {

// The constant-velocity Kalman filter, registered under "kalman.cv".
//
// State x = [e, n, v_e, v_n] in a local east/north tangent plane (meters),
// anchored at the first fix, so the filter is fully linear (no Jacobians) and
// tuned in meters rather than degrees, which are not a Euclidean space. It
// updates from GNSS position measurements (IHS_MEAS_POSITION_LLA) with a
// continuous white-noise-acceleration process model, and estimate(t) predicts
// the state forward to @t, so a consumer polling at UI rate gets a fresh,
// smooth position between 1 Hz fixes and a stationary vehicle stops wandering.
//
// A single measurement whose normalized innovation squared exceeds the 2-DOF
// chi-square gate is rejected as an outlier; a persistent run of them resets
// the filter to the raw measurement (a diverged filter is worse than none). The
// anchor is moved once the state drifts past ~10 km, keeping the flat-earth
// approximation honest.
//
// config (optional, may be NULL/empty): "q=<value>" sets the process-noise
// spectral density (m^2/s^3), the one manoeuvring tuning knob; the default
// suits a road vehicle.

// Register the filter under "kalman.cv" in the process-wide location registry.
// Idempotent (re-registration replaces the entry). Returns true on success.
bool RegisterKalmanCvFilter();

// The filter's ops table, exposed so it can be unit-tested directly (create /
// update / estimate / destroy) without going through the registry or a Manager.
const IhsLocationFilterOps& KalmanCvFilterOps();

}  // namespace ihs::location

#endif  // IHS_LOC_KALMAN_CV_H_
