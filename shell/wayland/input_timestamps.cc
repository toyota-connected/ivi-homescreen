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

#include "wayland/input_timestamps.h"

#include <algorithm>
#include <ctime>

#include <wayland-client.h>

#include <wl/client_helpers.hpp>
#include <wl/proxy_impl.hpp>

#include "logging/logging.h"

namespace ivi {

namespace itc = input_timestamps_unstable_v1::client;

namespace {

// Freshness bound for the clock-domain sanity check. An input timestamp is
// stamped by the kernel at event receipt, so in the CLOCK_MONOTONIC domain it
// is always in the recent past; a value outside this window (e.g. a
// CLOCK_REALTIME epoch value, or garbage) cannot be engine-clock-compatible.
// Generous enough to absorb scheduling delay and dispatch backlog.
constexpr uint64_t kMaxAgeUs = 10ull * 1000 * 1000;  // 10 s
// Small forward slack for rounding at the µs boundary.
constexpr uint64_t kMaxFutureUs = 1000;  // 1 ms

uint64_t MonotonicNowUs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000u +
         static_cast<uint64_t>(ts.tv_nsec) / 1000u;
}

}  // namespace

bool InputTimestamps::TryBindGlobal(wl_registry* registry,
                                    const uint32_t name,
                                    const std::string_view interface,
                                    const uint32_t version) {
  if (interface !=
      itc::zwp_input_timestamps_manager_v1_traits::wl_iface().name) {
    return false;
  }
  const uint32_t bind_ver =
      std::min(version, itc::zwp_input_timestamps_manager_v1_traits::version);
  auto* raw = static_cast<wl_proxy*>(wl_registry_bind(
      registry, name, &itc::zwp_input_timestamps_manager_v1_traits::wl_iface(),
      bind_ver));
  if (raw == nullptr) {
    spdlog::warn("[InputTimestamps] manager bind failed");
    return true;  // consumed (nobody else wants this global)
  }
  // The manager has no events; adopt without a listener.
  manager_.Attach(raw);
  spdlog::debug("[InputTimestamps] bound zwp_input_timestamps_manager_v1 v{}",
                bind_ver);
  // Registry global order vs. wl_seat.capabilities order is not guaranteed;
  // subscribe anything the seat already announced.
  if (pointer_ != nullptr) {
    AttachPointer(pointer_);
  }
  if (touch_ != nullptr) {
    AttachTouch(touch_);
  }
  return true;
}

void InputTimestamps::AttachPointer(wl_pointer* pointer) {
  pointer_ = pointer;
  Subscribe(pointer_sub_, reinterpret_cast<wl_proxy*>(pointer),
            &pending_pointer_, Kind::kPointer);
}

void InputTimestamps::AttachTouch(wl_touch* touch) {
  touch_ = touch;
  Subscribe(touch_sub_, reinterpret_cast<wl_proxy*>(touch), &pending_touch_,
            Kind::kTouch);
}

void InputTimestamps::Subscribe(wl::WlPtr<Subscription>& sub,
                                wl_proxy* device,
                                Pending* slot,
                                const Kind kind) {
  // Drop any previous subscription first — on device re-create (capability
  // bounce) the old zwp_input_timestamps_v1 became inert when its input
  // device was destroyed; on detach this is the whole job.
  sub.Reset();
  slot->valid = false;
  if (device == nullptr || manager_.IsNull()) {
    return;
  }
  // get_*_timestamps(id: new_id, device) — new_id leads ("no"), so plain
  // wl::construct (the opcode is a compile-time template argument, hence one
  // call per Kind).
  wl_proxy* raw =
      kind == Kind::kPointer
          ? wl::construct<itc::zwp_input_timestamps_v1_traits,
                          itc::zwp_input_timestamps_manager_v1_traits::Op::
                              GetPointerTimestamps>(*manager_.Get(), device)
          : wl::construct<itc::zwp_input_timestamps_v1_traits,
                          itc::zwp_input_timestamps_manager_v1_traits::Op::
                              GetTouchTimestamps>(*manager_.Get(), device);
  const char* what = kind == Kind::kPointer ? "pointer" : "touch";
  if (!wl::SetupHandler(sub, raw)) {
    spdlog::warn("[InputTimestamps] {} subscription failed", what);
    return;
  }
  sub.Get()->slot_ = slot;
  spdlog::debug("[InputTimestamps] subscribed {} timestamps", what);
}

uint64_t InputTimestamps::TakePointerTimeUs() {
  return Take(pending_pointer_);
}

uint64_t InputTimestamps::TakeTouchTimeUs() {
  return Take(pending_touch_);
}

uint64_t InputTimestamps::Take(Pending& pending) {
  if (!pending.valid) {
    return 0;
  }
  pending.valid = false;
  // Unsigned multiply may wrap for an absurd (hostile/corrupt) sec value;
  // wrapping is defined, and any wrapped result lands outside the freshness
  // window below, so the gate defuses overflow as well as wrong-domain
  // values.
  const uint64_t ts_us =
      pending.sec * 1'000'000u + static_cast<uint64_t>(pending.nsec) / 1000u;
  // Clock-domain sanity check (see class comment): a CLOCK_MONOTONIC input
  // timestamp is in the recent past relative to CLOCK_MONOTONIC now.
  const uint64_t now_us = MonotonicNowUs();
  if (ts_us > now_us + kMaxFutureUs || ts_us + kMaxAgeUs < now_us) {
    if (!clock_warned_) {
      clock_warned_ = true;
      spdlog::warn(
          "[InputTimestamps] timestamp {} µs is not CLOCK_MONOTONIC-domain "
          "(now {} µs); falling back to arrival-time stamping",
          ts_us, now_us);
    }
    return 0;
  }
  return ts_us;
}

void InputTimestamps::Reset() {
  pointer_sub_.Reset();
  touch_sub_.Reset();
  manager_.Reset();
  pointer_ = nullptr;
  touch_ = nullptr;
  pending_pointer_.valid = false;
  pending_touch_.valid = false;
}

}  // namespace ivi
