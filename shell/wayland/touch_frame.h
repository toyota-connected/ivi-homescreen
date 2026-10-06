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

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shell/input/touch_event.h"

namespace ihs {

/**
 * @brief Accumulates wl_touch contact updates into one batch per hardware scan
 *
 * `wl_touch.frame` is the protocol's atomicity boundary: everything between two
 * frame events is one logically-simultaneous scan. Holding the contacts until
 * the frame hands a 10-finger update to the engine as one group -- one lock,
 * one main-loop wake, one FlutterEngineSendPointerEvent -- instead of ten
 * submissions that can be split mid-scan.
 *
 * ## Why there is no compositor heuristic here
 *
 * weston <= 11 clears the touch focus in its up handler, so the frame that
 * should follow an up has no client left and is never sent (weston!980, fixed
 * in 12.0.0; the backport label on that MR never landed). The release would sit
 * in the accumulator until the next contact event.
 *
 * #714 handled that by flushing on up until a frame was seen to follow one,
 * tracked with two booleans. That only works for a single contact. A real
 * multi-touch scan is `up(A) motion(B) motion(C) frame`, and the motion between
 * the up and the frame cleared the "an up is outstanding" flag -- so on a
 * *fixed* compositor the latch never fired, the fallback stayed armed for the
 * session, and every up split the scan. Ten fingers with two lifting arrived as
 * three batches under three timestamps, which is exactly the skew
 * Engine::CoalesceTouchFrame exists to prevent.
 *
 * The signal that was wanted is not "was an up the last event" but "has this
 * scan ended", and the event loop already knows: when a dispatch round drains,
 * every event of the scan has been delivered. So `Frame()` flushes, and
 * `EndOfDispatch()` flushes whatever a frame never closed. No compositor
 * detection, and correct for any number of contacts.
 */
class TouchFrame {
 public:
  /// Receives one complete scan. @p frame_time_us is the shared high-resolution
  /// stamp, or 0 when no contact in the batch carried one.
  using Sink = std::function<void(const TouchEvent* events,
                                  std::size_t count,
                                  uint64_t frame_time_us)>;

  /// @param sink       receives each complete scan
  /// @param max_batch  cap on accumulated updates. wl_touch.frame is the normal
  ///                   boundary, but nothing in the protocol forces a
  ///                   compositor to send one, so a stream without frame
  ///                   markers must not grow this unbounded (parity with the
  ///                   drm and software seats). 0 disables the cap.
  TouchFrame(Sink sink, std::size_t max_batch)
      : sink_(std::move(sink)), max_batch_(max_batch) {}

  /// A new contact. @p ts_us is its high-resolution stamp, 0 if none.
  void Down(int32_t id, double x, double y, uint64_t ts_us);

  /// Contact moved. Ignored for an id with no prior Down -- the engine was
  /// never told that device went down.
  void Motion(int32_t id, double x, double y, uint64_t ts_us);

  /// Contact lifted. wl_touch.up carries no coordinates, so the contact's last
  /// position is reused. Ignored for an unknown id, as Motion is.
  void Up(int32_t id, uint64_t ts_us);

  /// wl_touch.cancel: the compositor took the gesture. Every active contact is
  /// canceled at its last position and the batch goes out immediately -- the
  /// protocol promises no frame after cancel. A contact left in the down phase
  /// trips FML_DCHECK(!state.is_down) in the engine's packet converter on the
  /// next session's down for that device.
  void Cancel();

  /// wl_touch.frame: the scan is complete.
  void Frame();

  /// The event-loop dispatch round drained. Flushes a scan whose frame never
  /// came; free when one did, since the accumulator is already empty.
  void EndOfDispatch() { Flush(); }

  /// Deliver whatever has accumulated. Used directly when contacts would
  /// otherwise straddle two engines within one batch.
  void Flush();

  /// Contacts currently down.
  [[nodiscard]] std::size_t active() const { return points_.size(); }
  /// Updates accumulated but not yet delivered.
  [[nodiscard]] std::size_t pending() const { return frame_.size(); }

 private:
  void Push(const TouchEvent& ev, uint64_t ts_us);

  Sink sink_;
  /// Active contacts: wl_touch id -> last position. A map, not an array indexed
  /// by id: compositor-assigned ids are unique per session, not bounded by the
  /// slot count, so indexing an array by id is an out-of-bounds write waiting
  /// for finger churn on a 10-slot controller.
  std::unordered_map<int32_t, std::pair<double, double>> points_;
  std::vector<TouchEvent> frame_;
  /// First nonzero stamp taken since the last flush. Contacts in one frame are
  /// logically simultaneous, so they share it; 0 means arrival-time stamping.
  uint64_t frame_time_us_{0};
  std::size_t max_batch_{0};
};

}  // namespace ihs
