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

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace wl_vulkan {

/**
 * @brief A surface size published from one thread and consumed on another,
 *        with a "changed since you last looked" flag.
 *
 * The Wayland event thread learns the surface size (an output resize, a scale
 * change) while the raster thread is the only one allowed to issue requests on
 * the wl_surface. So the writer records and flags; the reader decides what to
 * do and when. Extracted from WaylandVulkanBackend so that decision sequence
 * is testable without a compositor -- the bug it exists to prevent was a
 * declaration issued from the wrong thread, which no unit test could have
 * caught while it lived inline.
 *
 * Sizes are in SURFACE coordinates, which is not the buffer extent on a scaled
 * output; see WaylandWindow::DeclareSurfaceMapping.
 *
 * Ordering, which is what the tests pin: Set() stores the size and then raises
 * the flag; TakeIfStale() lowers the flag and then reads the size. A Set()
 * landing between a reader's two steps therefore yields the newer size and
 * leaves the flag raised, costing one redundant re-read rather than a missed
 * one. Losing an update is the failure that matters here; a duplicate is free.
 */
class SurfaceSizeLatch {
 public:
  using Size = std::pair<int32_t, int32_t>;

  /// Record @p width x @p height. Raises the stale flag only when the size
  /// actually moved, so an idempotent re-declaration costs the reader nothing.
  /// Returns whether it moved.
  bool Set(const int32_t width, const int32_t height) {
    {
      const std::lock_guard<std::mutex> lock(mu_);
      if (width == w_ && height == h_) {
        return false;
      }
      w_ = width;
      h_ = height;
    }
    // After the store, so a reader that sees the flag sees at least this size.
    stale_.store(true, std::memory_order_release);
    return true;
  }

  /// The size if it has moved since the last take, else nullopt. Lowers the
  /// flag. The lock-free check comes first: this runs once per present, and
  /// the common answer is "nothing to do".
  std::optional<Size> TakeIfStale() {
    if (!stale_.exchange(false, std::memory_order_acq_rel)) {
      return std::nullopt;
    }
    return Current();
  }

  /// The size regardless of the flag, for a caller that is about to use it
  /// anyway. Does not lower the flag -- see ClearStale().
  [[nodiscard]] Size Current() const {
    const std::lock_guard<std::mutex> lock(mu_);
    return {w_, h_};
  }

  /// Lower the flag without reading. For a path that is about to apply the
  /// current size itself, so the reader above does not then apply it twice.
  void ClearStale() { stale_.store(false, std::memory_order_release); }

  /// Whether a size has ever been recorded. Nothing can be declared before
  /// the window has mapped the surface, and that is not an error.
  [[nodiscard]] bool Valid() const {
    const auto [w, h] = Current();
    return w > 0 && h > 0;
  }

 private:
  mutable std::mutex mu_;
  int32_t w_{0};
  int32_t h_{0};
  // Separate from the mutex so the per-present check stays lock-free. A mutex
  // for the pair because it must be read consistently, and a packed atomic
  // would hide a shift-and-mask error as a wrong-sized region.
  std::atomic<bool> stale_{false};
};

}  // namespace wl_vulkan
