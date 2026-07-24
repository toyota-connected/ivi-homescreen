/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "fallback_source.hpp"

#include <utility>

namespace ihs::location {

FallbackSource::FallbackSource(std::unique_ptr<IEventSource> primary,
                               std::unique_ptr<IEventSource> fallback,
                               uint64_t primary_stale_ns)
    : primary_(std::move(primary)),
      fallback_(std::move(fallback)),
      primary_stale_ns_(primary_stale_ns) {}

FallbackSource::~FallbackSource() {
  Stop();
}

void FallbackSource::SetOnFix(FixSink on_fix) {
  on_fix_ = std::move(on_fix);
}

bool FallbackSource::Start() {
  if (started_) {
    return true;
  }
  // Wire the children to our handlers before starting them, so no child fix can
  // arrive before on_fix_ and the bookkeeping are in place.
  if (primary_) {
    primary_->SetOnFix([this](const Position& p) { OnPrimary(p); });
  }
  if (fallback_) {
    fallback_->SetOnFix([this](const Position& p) { OnFallback(p); });
  }
  if (primary_) {
    primary_->Start();
  }
  if (fallback_) {
    fallback_->Start();
  }
  started_ = true;
  // Return true even if neither is producing yet: a source may report later,
  // matching the enum providers' "handle returned, no fix yet" contract.
  return true;
}

void FallbackSource::Stop() {
  // Stop the children (each joins its worker), so no OnPrimary/OnFallback is in
  // flight afterward.
  if (primary_) {
    primary_->Stop();
  }
  if (fallback_) {
    fallback_->Stop();
  }
  started_ = false;
}

void FallbackSource::OnPrimary(const Position& p) {
  {
    const std::lock_guard<std::mutex> lock(mu_);
    last_primary_ns_ = p.t_monotonic_ns;
    primary_seen_ = true;
  }
  // The primary is authoritative: forward every primary fix.
  if (on_fix_) {
    on_fix_(p);
  }
}

void FallbackSource::OnFallback(const Position& p) {
  bool forward = false;
  {
    const std::lock_guard<std::mutex> lock(mu_);
    // Forward the fallback only when the primary has never produced, or has
    // been silent longer than the staleness window as of THIS fix's arrival
    // time. Evaluated at the event, by timestamp — no timer notices the
    // silence.
    forward = !primary_seen_ ||
              (p.t_monotonic_ns > last_primary_ns_ &&
               p.t_monotonic_ns - last_primary_ns_ > primary_stale_ns_);
  }
  if (forward && on_fix_) {
    on_fix_(p);
  }
}

}  // namespace ihs::location
