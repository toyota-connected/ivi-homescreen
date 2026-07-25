/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef IHS_LOC_FALLBACK_SOURCE_H_
#define IHS_LOC_FALLBACK_SOURCE_H_

#include <cstdint>
#include <memory>
#include <mutex>

#include "location.hpp"

namespace ihs::location {

// Event-driven gpsd-primary / geoclue-fallback combiner (the "auto" source).
// It forwards the primary's fixes while the primary is producing them, and the
// fallback's only while the primary has gone quiet.
//
// The staleness decision is EVENT-DRIVEN, not timed: on each fallback fix it
// compares that fix's monotonic arrival time to the primary's last arrival, and
// forwards it only if the primary has been silent longer than @primary_stale.
// No timer is needed to notice the primary going quiet — the check happens when
// the next fallback fix arrives (you detect silence at the next event, by
// timestamp). If both go quiet there is simply no event and nothing to forward.
// This reproduces the old poll-time FallbackLocationProvider decision without a
// poll.
//
// Threading: the children invoke OnPrimary/OnFallback from their own worker
// threads; a mutex guards the primary-freshness bookkeeping. The outer sink is
// invoked outside the lock (the Manager it feeds is itself thread-safe).
class FallbackSource : public IEventSource {
 public:
  static constexpr uint64_t kDefaultStaleNs = 5'000'000'000ULL;  // 5 s

  FallbackSource(std::unique_ptr<IEventSource> primary,
                 std::unique_ptr<IEventSource> fallback,
                 uint64_t primary_stale_ns = kDefaultStaleNs);
  ~FallbackSource() override;

  FallbackSource(const FallbackSource&) = delete;
  FallbackSource& operator=(const FallbackSource&) = delete;

  void SetOnFix(FixSink on_fix) override;  // before Start()
  bool Start() override;
  void Stop() override;

 private:
  void OnPrimary(const Position& p);
  void OnFallback(const Position& p);

  const std::unique_ptr<IEventSource> primary_;
  const std::unique_ptr<IEventSource> fallback_;
  const uint64_t primary_stale_ns_;

  FixSink on_fix_;  // set before Start(); read on the child worker threads

  std::mutex mu_;
  uint64_t last_primary_ns_ = 0;  // guarded by mu_
  bool primary_seen_ = false;     // guarded by mu_
  bool started_ = false;
};

}  // namespace ihs::location

#endif  // IHS_LOC_FALLBACK_SOURCE_H_
