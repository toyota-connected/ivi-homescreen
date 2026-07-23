/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "fallback_location_provider.hpp"

#include <utility>

namespace ihs::location {

FallbackLocationProvider::FallbackLocationProvider(
    std::unique_ptr<ILocationProvider> primary,
    std::unique_ptr<ILocationProvider> fallback,
    std::chrono::steady_clock::duration primary_stale)
    : primary_(std::move(primary)),
      fallback_(std::move(fallback)),
      primary_stale_(primary_stale) {}

bool FallbackLocationProvider::Start() {
  bool ok = false;
  if (primary_) {
    ok = primary_->Start() || ok;
  }
  if (fallback_) {
    ok = fallback_->Start() || ok;
  }
  return ok;
}

void FallbackLocationProvider::Stop() {
  if (primary_) {
    primary_->Stop();
  }
  if (fallback_) {
    fallback_->Stop();
  }
}

bool FallbackLocationProvider::Latest(Position& out) {
  if (primary_) {
    const uint64_t g = primary_->generation();
    if (g != primary_seen_gen_) {
      primary_seen_gen_ = g;
      primary_ever_ = true;
      primary_last_change_ = std::chrono::steady_clock::now();
    }
    Position pp;
    const bool fresh = primary_ever_ && (std::chrono::steady_clock::now() -
                                         primary_last_change_) < primary_stale_;
    if (fresh && primary_->Latest(pp)) {
      out = pp;
      return true;
    }
  }
  return fallback_ && fallback_->Latest(out);
}

uint64_t FallbackLocationProvider::generation() const {
  // Monotonic across both children: any new fix from either bumps this, so the
  // consumer re-evaluates (Latest() then picks the right source).
  return (primary_ ? primary_->generation() : 0) +
         (fallback_ ? fallback_->generation() : 0);
}

}  // namespace ihs::location
