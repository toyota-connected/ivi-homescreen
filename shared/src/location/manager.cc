/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

// The location Manager (see manager.hpp): binds registered sources, fuses the
// measurements they push into a Position, and serves it through
// ILocationProvider. Not yet on the ihs_location_start() path — exercised only
// by manager_test until the gpsd/geoclue sources are registered.

#include "manager.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>

#include "registry.hpp"

namespace ihs::location {

namespace {

// 1-sigma from a per-component variance (1-sigma^2); < 0 stays "unknown".
double SigmaFromVariance(double variance) {
  return variance >= 0.0 ? std::sqrt(variance) : -1.0;
}

// Copy the C-ABI IhsPosition a filter fills into the internal Position.
void FromIhsPosition(Position& out, const IhsPosition& in) {
  out.latitude = in.latitude;
  out.longitude = in.longitude;
  out.bearing_deg = in.bearing_deg;
  out.speed_mps = in.speed_mps;
  out.mode = in.mode;
  out.has_bearing = in.has_bearing != 0;
  out.t_monotonic_ns = in.t_monotonic_ns;
  out.sigma_e_m = in.sigma_e_m;
  out.sigma_n_m = in.sigma_n_m;
  out.sigma_v_mps = in.sigma_v_mps;
}

}  // namespace

Manager::Manager(std::vector<std::string> source_keys,
                 std::string filter_key,
                 std::string filter_config)
    : source_keys_(std::move(source_keys)),
      filter_key_(std::move(filter_key)),
      filter_config_(std::move(filter_config)) {}

Manager::~Manager() {
  Shutdown();
}

bool Manager::Start() {
  if (started_) {
    return true;
  }

  // Resolve the optional filter first, so its instance exists before any
  // measurement can arrive from a source.
  if (!filter_key_.empty()) {
    FilterEntry fe;
    if (LookupFilter(filter_key_, fe) && fe.ops.create != nullptr) {
      // create() is a user callback, so run it outside the lock; publish the
      // resolved filter under the lock so a concurrent Latest() never reads a
      // torn pointer. Sources are not started yet, so no measurement races it.
      void* const inst = fe.ops.create(
          fe.user_data,
          filter_config_.empty() ? nullptr : filter_config_.c_str());
      const std::lock_guard<std::mutex> lock(mutex_);
      filter_ops_ = fe.ops;
      filter_user_data_ = fe.user_data;
      filter_instance_ = inst;
    }
    // A configured-but-missing (or create-failed) filter leaves
    // filter_instance_ null, so the built-in passthrough is used instead of
    // failing the whole service.
  }

  // Bind each registered source key (skipping the rest), then start them.
  for (const std::string& key : source_keys_) {
    SourceEntry se;
    if (LookupSource(key, se)) {
      sources_.push_back(BoundSource{se.ops, se.user_data, false});
    }
  }
  for (BoundSource& s : sources_) {
    if (s.ops.start != nullptr &&
        s.ops.start(s.user_data, &SinkThunk, this) == 1) {
      s.started = true;
    }
  }

  // Return true even if no bound source has produced a fix yet: a live source
  // may only start reporting later, matching the enum providers' "handle
  // returned, no fix yet" contract. Sources bind once here and are not
  // re-queried, so registering one after Start() has no effect.
  started_ = true;
  return true;
}

void Manager::Stop() {
  Shutdown();
}

void Manager::Shutdown() {
  // Stop sources before tearing down the filter so no measurement can arrive
  // after the filter instance is destroyed. A source's stop() joins its worker,
  // so no sink call is in flight once it returns.
  for (BoundSource& s : sources_) {
    if (s.started && s.ops.stop != nullptr) {
      s.ops.stop(s.user_data);
    }
    s.started = false;
  }
  sources_.clear();

  // Detach the filter under the lock (a concurrent Latest() reads it there) and
  // reset have_fix_ so a restarted Manager cannot report a pre-stop fix; then
  // destroy the instance outside the lock (destroy() is a user callback).
  void* inst = nullptr;
  IhsLocationFilterOps ops{};
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    inst = filter_instance_;
    ops = filter_ops_;
    filter_instance_ = nullptr;
    have_fix_ = false;
  }
  if (inst != nullptr && ops.destroy != nullptr) {
    ops.destroy(inst);
  }
  started_ = false;
}

void Manager::SinkThunk(void* sink_user_data, const IhsMeasurement* m) {
  if (m == nullptr) {
    return;
  }
  // Bounded copy across the ABI boundary (the #337 discipline the header
  // promises and the registry ops already apply): a source may be built against
  // a different IhsMeasurement layout, so copy only min(its struct_size, ours)
  // into a zeroed full-size struct and never read past the source's object. A
  // struct too small to carry the value/variance block cannot describe a fix,
  // so it is dropped rather than read speculatively.
  IhsMeasurement full{};
  const size_t n =
      m->struct_size < sizeof(full) ? m->struct_size : sizeof(full);
  if (n < offsetof(IhsMeasurement, flags)) {
    return;
  }
  std::memcpy(&full, m, n);
  full.struct_size = n;
  static_cast<Manager*>(sink_user_data)->OnMeasurement(full);
}

void Manager::OnMeasurement(const IhsMeasurement& m) {
  // Drop an unusable position up front so neither the filter nor the
  // passthrough ever sees NaN/out-of-range coordinates, and so
  // have_fix_/generation_ below never mark a fix that was not actually accepted
  // (which would let Latest() report the zero-initialized default, or bump
  // generation over a stale fix). Other measurement kinds carry their own
  // guards in ApplyToPassthrough.
  if (m.kind == IHS_MEAS_POSITION_LLA &&
      (m.value_count < 2 || !ValidLatLon(m.value[0], m.value[1]))) {
    return;
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  if (filter_instance_ != nullptr && filter_ops_.update != nullptr) {
    filter_ops_.update(filter_instance_, &m);
  } else {
    ApplyToPassthrough(m);
  }
  // A position measurement is the anchor of a fix; count one generation per
  // accepted position (velocity/heading augment the same fix silently), so a
  // consumer sees one bump per fix regardless of how many components a source
  // splits it into.
  if (m.kind == IHS_MEAS_POSITION_LLA) {
    have_fix_ = true;
    ++generation_;
  }
}

void Manager::ApplyToPassthrough(const IhsMeasurement& m) {
  switch (m.kind) {
    case IHS_MEAS_POSITION_LLA:
      // OnMeasurement has already validated value_count >= 2 and the
      // coordinates, so store them directly.
      latest_.latitude = m.value[0];
      latest_.longitude = m.value[1];
      // A third component (altitude) marks a 3D fix, mirroring gpsd's
      // mode 2 (lat/lon) vs mode 3 (lat/lon/alt).
      latest_.mode = m.value_count >= 3 ? 3 : 2;
      latest_.t_monotonic_ns = m.t_monotonic_ns;
      latest_.sigma_e_m = SigmaFromVariance(m.variance[0]);
      latest_.sigma_n_m = SigmaFromVariance(m.variance[1]);
      break;
    case IHS_MEAS_SPEED:
      if (m.value_count >= 1) {
        latest_.speed_mps = m.value[0];
        latest_.sigma_v_mps = SigmaFromVariance(m.variance[0]);
      }
      break;
    case IHS_MEAS_HEADING:
      if (m.value_count >= 1) {
        latest_.bearing_deg = m.value[0];
        latest_.has_bearing = true;
      }
      break;
    default:
      // VELOCITY_NED / YAW_RATE / ACCEL_BODY are filter inputs; the passthrough
      // reports only what maps directly onto a fix.
      break;
  }
}

bool Manager::Latest(Position& out) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (filter_instance_ != nullptr && filter_ops_.estimate != nullptr) {
    IhsPosition est{};
    if (filter_ops_.estimate(filter_instance_, MonotonicNs(), &est) == 1) {
      FromIhsPosition(out, est);
      return true;
    }
    return false;
  }
  if (!have_fix_) {
    return false;
  }
  out = latest_;
  return true;
}

uint64_t Manager::generation() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return generation_;
}

}  // namespace ihs::location
