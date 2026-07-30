/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "vsync/consumer_paced_vsync.h"

#include <chrono>
#include <ctime>

#include "asio/bind_executor.hpp"
#include "asio/post.hpp"

#include "logging/logging.h"
#include "task_runner.h"

namespace ivi {

namespace {
uint64_t MonotonicNs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}
}  // namespace

ConsumerPacedVsyncSource::ConsumerPacedVsyncSource(IVsyncProvider& provider,
                                                   uint32_t min_period_ns,
                                                   uint32_t pipeline_depth)
    : provider_(provider),
      min_period_ns_(min_period_ns),
      pipeline_depth_(pipeline_depth == 0 ? 1 : pipeline_depth) {}

ConsumerPacedVsyncSource::~ConsumerPacedVsyncSource() {
  Stop();
}

void ConsumerPacedVsyncSource::Start(FLUTTER_API_SYMBOL(FlutterEngine) engine,
                                     TaskRunner* runner) {
  if (running_.load() || runner == nullptr ||
      runner->GetIoContext() == nullptr) {
    return;
  }
  runner_ = runner;
  credits_ = static_cast<int>(pipeline_depth_);  // all slots free at bring-up
  last_deliver_ns_ = 0;
  ceiling_armed_ = false;
  timer_ = std::make_unique<asio::steady_timer>(*runner_->GetIoContext());
  // Batons must always park (this source, not an inline drain, decides timing).
  provider_.SetSourcePending(true);
  provider_.SetPeriodNs(min_period_ns_ != 0 ? min_period_ns_ : 16'666'667u);
  provider_.SetEngine(engine, runner_);
  running_.store(true, std::memory_order_release);
  ihs::log::info("[ConsumerPacedVsync] paced vsync: depth={} ceiling={}",
                 pipeline_depth_,
                 min_period_ns_ != 0
                     ? std::to_string(1'000'000'000u / min_period_ns_) + "fps"
                     : std::string("none"));
}

void ConsumerPacedVsyncSource::SubmitBaton(FLUTTER_API_SYMBOL(FlutterEngine)
                                               engine,
                                           intptr_t baton) {
  // Park the baton (SetSourcePending(true) keeps SubmitBaton from draining it
  // inline), then try to deliver on the strand under the credit + rate rules.
  provider_.SubmitBaton(engine, baton);
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }
  asio::post(*runner_->GetStrandContext(), [this]() { TryDeliver(); });
}

void ConsumerPacedVsyncSource::AddCredit() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }
  asio::post(*runner_->GetStrandContext(), [this]() {
    if (credits_ < static_cast<int>(pipeline_depth_)) {
      ++credits_;
    }
    TryDeliver();
  });
}

void ConsumerPacedVsyncSource::TryDeliver() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }
  // Nothing to do unless a slot is free AND the engine has parked a baton.
  if (credits_ <= 0 || !provider_.HasParkedBaton()) {
    return;
  }
  const uint64_t now = MonotonicNs();
  if (min_period_ns_ != 0 && last_deliver_ns_ != 0) {
    const uint64_t elapsed = now - last_deliver_ns_;
    if (elapsed < min_period_ns_) {
      ArmCeiling(min_period_ns_ - elapsed);  // rate ceiling: deliver later
      return;
    }
  }
  if (provider_.DeliverParkedBaton()) {
    --credits_;  // this frame commits one pipeline slot
    last_deliver_ns_ = now;
  }
}

void ConsumerPacedVsyncSource::ArmCeiling(uint64_t ns) {
  if (ceiling_armed_) {
    return;  // a wait is already pending; its handler will re-check
  }
  ceiling_armed_ = true;
  timer_->expires_after(std::chrono::nanoseconds(ns));
  timer_->async_wait(asio::bind_executor(
      *runner_->GetStrandContext(), [this](const asio::error_code& ec) {
        ceiling_armed_ = false;
        if (ec || !running_.load(std::memory_order_acquire)) {
          return;  // cancelled (teardown) or io_context stopped
        }
        TryDeliver();
      }));
}

void ConsumerPacedVsyncSource::Stop() {
  if (running_.exchange(false, std::memory_order_acq_rel) &&
      runner_ != nullptr && runner_->GetStrandContext() != nullptr) {
    asio::post(*runner_->GetStrandContext(), [this]() {
      if (timer_ != nullptr) {
        timer_->cancel();
      }
    });
  }
  provider_.Stop();
}

}  // namespace ivi
