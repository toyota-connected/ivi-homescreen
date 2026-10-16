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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace wl_vulkan {

/// Upper bound on the wait for a wl_surface.frame callback. An occluded or
/// unmapped surface stops receiving callbacks; this degrades it to ~10 fps
/// instead of hanging the rasterizer.
inline constexpr std::chrono::milliseconds kFrameWaitCap{100};

/**
 * @brief When a dma-buf present stops waiting for the previous commit's
 *        wl_surface.frame callback.
 *
 * The callback is the dma-buf path's backpressure, but it is a poor clock on a
 * compositor that sends it late: one that fires it after its repaint finishes,
 * rather than when it is ready for the next buffer, holds the rasterizer for
 * the length of that repaint on top of the refresh. The wait exists to keep
 * the producer from overwriting a buffer the compositor still holds and from
 * committing faster than it repaints. When a ring slot is already released,
 * the first concern is gone, so the wait only has to cover the second: it
 * ends once a fraction of a refresh has passed since the last commit, or at
 * the callback, whichever comes first.
 *
 * The fraction is below one so jitter in when the frame starts does not cost
 * a refresh, and well above one half so two commits cannot land in the same
 * repaint, where the first would be discarded unseen.
 *
 * Only while callbacks are arriving, late or not: one must have landed within
 * the last two refreshes. An occluded surface gets none, but its old buffers
 * are still released, so without this it would render at nearly the refresh
 * rate instead of degrading to the cap. This assumes the compositor stops
 * callbacks to a surface it does not show; one that keeps sending them gets
 * up to 4/3 of the refresh rate here instead of the callback rate. For the
 * first two refreshes after callbacks stop, a frame or two still goes out
 * early.
 *
 * The window is relative to the refresh while a compositor's lateness is not.
 * On an output fast enough that a late callback is older than two refreshes,
 * this falls back to the plain callback wait: no gain, no harm.
 *
 * The floor holds only if @p refresh_ns is not shorter than the real period.
 * After a mode change that lowers the refresh rate, the cached period is stale
 * until the next presentation, so one frame may be discarded.
 *
 * With no free slot, an unknown refresh period, no commit yet, or no recent
 * callback, this is the plain bounded wait.
 *
 * @param now            the current time
 * @param slot_free      whether a ring slot's buffer has been released
 * @param last_commit    when the previous frame was committed, if any
 * @param last_callback  when a frame callback last fired, if ever
 * @param refresh_ns     the output's refresh period; 0 when not yet known
 */
inline std::chrono::steady_clock::time_point FrameWaitDeadline(
    const std::chrono::steady_clock::time_point now,
    const bool slot_free,
    const std::optional<std::chrono::steady_clock::time_point> last_commit,
    const std::optional<std::chrono::steady_clock::time_point> last_callback,
    const uint32_t refresh_ns) {
  const auto cap = now + kFrameWaitCap;
  if (!slot_free || !last_commit.has_value() || !last_callback.has_value() ||
      refresh_ns == 0) {
    return cap;
  }
  const auto refresh = std::chrono::nanoseconds(refresh_ns);
  if (now - *last_callback >= 2 * refresh) {
    return cap;
  }
  return std::clamp(*last_commit + refresh * 3 / 4, now, cap);
}

}  // namespace wl_vulkan
