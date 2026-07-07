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

#ifndef SHELL_WAYLAND_INPUT_TIMESTAMPS_H_
#define SHELL_WAYLAND_INPUT_TIMESTAMPS_H_

#include <cstdint>
#include <string_view>

#include "input_timestamps_client.hpp"  // generated (input_timestamps_unstable_v1::client)

#include <wl/wl_ptr.hpp>

struct wl_pointer;
struct wl_registry;
struct wl_touch;

namespace ivi {

/**
 * @brief Owns zwp_input_timestamps_manager_v1 and the per-device
 * zwp_input_timestamps_v1 subscriptions, turning the protocol's
 * nanosecond-resolution timestamp events into microsecond timestamps for the
 * pointer/touch → Flutter path.
 *
 * Why: wl_pointer/wl_touch carry millisecond-granularity timestamps, which is
 * poor input for the framework's velocity tracker (fling feel) and pointer
 * resampler. The protocol delivers a high-resolution timestamp event
 * immediately BEFORE each input event it qualifies; Display's handlers
 * consume it via the take-and-clear accessors below and hand it to
 * Engine::CoalesceMouseEvent / CoalesceTouchFrame.
 *
 * Threading: every entry point — TryBindGlobal (registry enumeration),
 * AttachPointer/AttachTouch (wl_seat.capabilities), the OnTimestamp events,
 * and the Take*() accessors (wl_pointer/wl_touch handlers) — runs on the
 * Wayland event thread, so the pending slots need no locking (same contract
 * as Display::m_touch).
 *
 * Runtime-optional: when the compositor never advertises the manager,
 * everything no-ops and Take*() returns 0, which the engine treats as
 * "stamp with arrival time" — the historical behavior.
 *
 * Clock domain: the protocol guarantees only "the same clock domain as the
 * associated input event timestamp", which the core protocol leaves
 * unspecified (every mainstream compositor uses CLOCK_MONOTONIC — the engine
 * clock). Rather than trust that convention blindly, Take*() sanity-checks
 * each value against CLOCK_MONOTONIC now and falls back to 0 (arrival-time
 * stamping) when the value can't be from that domain, logging once.
 */
class InputTimestamps {
 public:
  InputTimestamps() = default;
  ~InputTimestamps() { Reset(); }

  InputTimestamps(const InputTimestamps&) = delete;
  InputTimestamps& operator=(const InputTimestamps&) = delete;

  /// Bind zwp_input_timestamps_manager_v1 if @p interface names it. Called
  /// from Display's registry loop alongside the vsync provider and shells.
  /// Subscribes any already-attached devices (registry global order vs.
  /// wl_seat.capabilities order is not guaranteed). Returns true if consumed.
  [[nodiscard]] bool TryBindGlobal(wl_registry* registry,
                                   uint32_t name,
                                   std::string_view interface,
                                   uint32_t version);

  /// Track the seat's wl_pointer. nullptr detaches (capability lost). If the
  /// manager is bound, (re)creates the pointer subscription.
  void AttachPointer(wl_pointer* pointer);

  /// Track the seat's wl_touch. nullptr detaches (capability lost). If the
  /// manager is bound, (re)creates the touch subscription.
  void AttachTouch(wl_touch* touch);

  /// Take-and-clear the pending pointer timestamp: microseconds in the
  /// engine clock domain (CLOCK_MONOTONIC), or 0 when there is none / the
  /// clock domain is unusable. The wl_pointer handlers must call this for
  /// EVERY timestamp-carrying event (motion/button/axis) — even ones they
  /// drop — so a stale stamp is never applied to a later event.
  [[nodiscard]] uint64_t TakePointerTimeUs();

  /// Take-and-clear the pending touch timestamp; semantics as above, for the
  /// wl_touch handlers (down/up/motion).
  [[nodiscard]] uint64_t TakeTouchTimeUs();

  /// Destroy the subscriptions + manager. Must run before
  /// wl_display_disconnect (proxy destruction touches the display).
  void Reset();

 private:
  // A pending (tv_sec, tv_nsec) captured by an OnTimestamp event, waiting for
  // the input event it qualifies.
  struct Pending {
    uint64_t sec = 0;
    uint32_t nsec = 0;
    bool valid = false;
  };

  // zwp_input_timestamps_v1 event handler; stores into the owner's slot.
  class Subscription final
      : public input_timestamps_unstable_v1::client::CZwpInputTimestampsV1<
            Subscription> {
   public:
    void OnTimestamp(uint32_t tv_sec_hi,
                     uint32_t tv_sec_lo,
                     uint32_t tv_nsec) override {
      if (slot_ != nullptr) {
        slot_->sec = (static_cast<uint64_t>(tv_sec_hi) << 32u) | tv_sec_lo;
        slot_->nsec = tv_nsec;
        slot_->valid = true;
      }
    }

    Pending* slot_ = nullptr;
  };

  // zwp_input_timestamps_manager_v1 has no events; concrete type for WlPtr.
  class Manager final : public input_timestamps_unstable_v1::client::
                            CZwpInputTimestampsManagerV1<Manager> {};

  // (Re)create / destroy the subscription for one device slot.
  enum class Kind : uint8_t { kPointer, kTouch };
  void Subscribe(wl::WlPtr<Subscription>& sub,
                 wl_proxy* device,
                 Pending* slot,
                 Kind kind);

  // Validate + convert a pending value; clears it. 0 when absent or the
  // clock domain check fails.
  [[nodiscard]] uint64_t Take(Pending& pending);

  wl::WlPtr<Manager> manager_;
  wl::WlPtr<Subscription> pointer_sub_;
  wl::WlPtr<Subscription> touch_sub_;

  // Raw devices as last announced by wl_seat.capabilities; not owned (the
  // Display owns/releases them). Kept so a manager bound after the
  // capabilities event can still subscribe.
  wl_pointer* pointer_ = nullptr;
  wl_touch* touch_ = nullptr;

  Pending pending_pointer_;
  Pending pending_touch_;

  // Log the clock-domain fallback once, not per event.
  bool clock_warned_ = false;
};

}  // namespace ivi

#endif  // SHELL_WAYLAND_INPUT_TIMESTAMPS_H_
