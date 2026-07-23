/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef IHS_LOC_FALLBACK_LOCATION_PROVIDER_H_
#define IHS_LOC_FALLBACK_LOCATION_PROVIDER_H_

#include <chrono>
#include <cstdint>
#include <memory>

#include "location.hpp"

namespace ihs::location {

// Combines a primary and a fallback provider: reports the primary's fix while
// the primary is producing FRESH fixes, and the fallback's otherwise. This is
// the design's "gpsd primary + geoclue fallback" — gpsd is authoritative when
// it has signal, and geoclue (network / Wi-Fi) fills in when gpsd goes quiet or
// was never present. Freshness is judged by how recently the primary's
// generation last advanced.
//
// Both children run their own worker threads; this object only reads them, and
// Latest()/generation() are called on the consumer (engine) thread, so its own
// bookkeeping needs no lock.
class FallbackLocationProvider final : public ILocationProvider {
 public:
  FallbackLocationProvider(std::unique_ptr<ILocationProvider> primary,
                           std::unique_ptr<ILocationProvider> fallback,
                           std::chrono::steady_clock::duration primary_stale =
                               std::chrono::seconds(5));
  ~FallbackLocationProvider() override =
      default;  // children stop in their dtors

  bool Start() override;
  void Stop() override;
  bool Latest(Position& out) override;
  [[nodiscard]] uint64_t generation() const override;

 private:
  std::unique_ptr<ILocationProvider> primary_;
  std::unique_ptr<ILocationProvider> fallback_;
  const std::chrono::steady_clock::duration primary_stale_;

  // Consumer-thread only (Latest()): track when the primary last advanced so a
  // stale primary yields to the fallback.
  uint64_t primary_seen_gen_ = 0;
  bool primary_ever_ = false;
  std::chrono::steady_clock::time_point primary_last_change_;
};

}  // namespace ihs::location

#endif  // IHS_LOC_FALLBACK_LOCATION_PROVIDER_H_
