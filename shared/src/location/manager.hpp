/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef IHS_LOC_MANAGER_H_
#define IHS_LOC_MANAGER_H_

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "ihs/location.h"
#include "location.hpp"

namespace ihs::location {

// Composes a location service from registered measurement sources and an
// optional filter — the runtime counterpart of the registry. It binds each
// source key (skipping any not registered), hands them a sink, and fuses the
// measurements they push into one Position that consumers poll through the
// ILocationProvider interface. That is deliberate: IhsLocationService can hold
// a Manager exactly as it holds a gpsd/geoclue provider today, so swapping the
// enum path onto the registry is a one-line change with no consumer impact.
//
// With no filter key a built-in passthrough assembles the latest measurements
// into a Position directly (today's behavior — the fix is reported as
// received). With a filter key, measurements are routed to the registered
// filter's update(), and Latest() asks it to estimate() the state at the
// current time.
//
// Threading: sources push from their own acquisition threads; a single mutex
// serializes measurement intake, the (non-thread-safe) filter instance, and the
// stored Position/generation that Latest()/generation() read. A source must not
// call the sink after its stop() returns — the same contract the gpsd/geoclue
// workers already honor (stop() joins the thread).
//
// Start()/Stop() are lifecycle calls on the owning thread and must not run
// concurrently with each other or with Latest()/generation() — the same
// contract as the C ABI handle they back, where ihs_location_stop() frees the
// service. Between Start() and Stop(), Latest()/generation() may be polled from
// any thread while sources push concurrently.
class Manager : public ILocationProvider {
 public:
  // @source_keys are bound in order, skipping any not registered. @filter_key
  // empty selects the built-in passthrough; otherwise the registered filter is
  // created with @filter_config (NULL when empty). A configured-but-missing (or
  // create-failed) filter falls back to the passthrough rather than failing.
  Manager(std::vector<std::string> source_keys,
          std::string filter_key,
          std::string filter_config);
  ~Manager() override;

  Manager(const Manager&) = delete;
  Manager& operator=(const Manager&) = delete;

  // Publish a complete fix directly and atomically. This is the path for a
  // built-in source (gpsd/geoclue) that already produces a whole Position:
  // storing it in one step avoids the assembly window a consumer polling on
  // generation() could otherwise observe if the fix were split into separate
  // POSITION/SPEED/HEADING measurements. The measurement path (OnMeasurement,
  // via a registered source + optional filter) remains for decomposed sources.
  // Thread-safe; typically called from the source's acquisition thread.
  void PublishFix(const Position& fix);

  // Install a callback fired once on each new fix (each generation bump), for a
  // push consumer. It is invoked WITHOUT the internal mutex held (so the
  // callback may call back into Latest()/generation()), on whichever thread
  // produced the fix. Pass nullptr to clear. Thread-safe.
  void SetFixNotify(std::function<void()> notify);

  bool Start() override;
  void Stop() override;
  bool Latest(Position& out) override;
  [[nodiscard]] uint64_t generation() const override;

  // Read the estimate at a specific CLOCK_MONOTONIC time. For the built-in
  // passthrough this returns the last fix unchanged (it is time-independent);
  // for a filter it runs estimate(@at_monotonic_ns), which predicts the state
  // forward to that instant. Latest() is exactly Estimate(out, MonotonicNs());
  // a caller passes an explicit time to sample the state on its own timeline
  // (e.g. a deterministic test clock, or to compensate pipeline latency).
  // Returns false until a fix exists. Thread-safe.
  bool Estimate(Position& out, uint64_t at_monotonic_ns);

 private:
  // C sink trampoline: sink_user_data is the Manager*.
  static void SinkThunk(void* sink_user_data, const IhsMeasurement* m);
  void OnMeasurement(const IhsMeasurement& m);
  // Fold one measurement into latest_ (built-in passthrough). Caller holds
  // mutex_.
  void ApplyToPassthrough(const IhsMeasurement& m);
  // Stop sources + destroy the filter. Idempotent; used by Stop() and the
  // destructor (non-virtual so the destructor does not call an override).
  void Shutdown();

  struct BoundSource {
    IhsLocationSourceOps ops{};
    void* user_data = nullptr;
    bool started = false;
  };

  const std::vector<std::string> source_keys_;
  const std::string filter_key_;
  const std::string filter_config_;

  std::vector<BoundSource> sources_;

  // Optional registered filter; filter_instance_ null => built-in passthrough.
  IhsLocationFilterOps filter_ops_{};
  void* filter_user_data_ = nullptr;
  void* filter_instance_ = nullptr;

  mutable std::mutex mutex_;
  Position latest_;                   // guarded by mutex_
  bool have_fix_ = false;             // guarded by mutex_
  uint64_t generation_ = 0;           // guarded by mutex_
  std::function<void()> fix_notify_;  // guarded by mutex_; invoked unlocked
  bool started_ = false;
};

}  // namespace ihs::location

#endif  // IHS_LOC_MANAGER_H_
