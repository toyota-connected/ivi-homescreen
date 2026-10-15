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
 * do and when. Extracted from WaylandVulkanBackend so that handoff is testable
 * without a compositor -- the two defects it exists to prevent were a region
 * sized in buffer rather than surface coordinates, and a declaration issued
 * from the writer's thread, neither of which a unit test could reach while
 * this lived inline.
 *
 * Sizes are in SURFACE coordinates, which is not the buffer extent on a scaled
 * output; see WaylandWindow::DeclareSurfaceMapping.
 *
 * Every method is one critical section, so there is no interleaving to reason
 * about and no ordering contract for a caller to get wrong. An earlier version
 * kept the flag in a separate atomic to keep the reader's check lock-free, and
 * paid for it with a store-then-raise / lower-then-read rule that could not be
 * tested: a mutant with the order reversed passed a 20000-write stress test
 * 5/5, because losing an update needs the writer's last Set to land inside a
 * nanosecond window. An uncontended lock once per present at 60Hz is tens of
 * nanoseconds, so the trade was never worth it.
 *
 * There is deliberately no "read without clearing": that pairing -- read, then
 * clear -- is exactly the lost-update order, and an API that offers it invites
 * the bug back. A caller that needs the size takes it.
 */
class SurfaceSizeLatch {
 public:
  using Size = std::pair<int32_t, int32_t>;

  /// Record @p width x @p height. Marks it unseen only when the size actually
  /// moved, so an idempotent re-declaration costs the reader nothing. Returns
  /// whether it moved.
  bool Set(const int32_t width, const int32_t height) {
    const std::lock_guard<std::mutex> lock(mu_);
    if (width == w_ && height == h_) {
      return false;
    }
    w_ = width;
    h_ = height;
    stale_ = true;
    return true;
  }

  /// The size if it has moved since the last take, else nullopt. Clears the
  /// flag. For a caller that acts only on a change -- the present path, which
  /// asks every frame and normally has nothing to do.
  std::optional<Size> TakeIfStale() {
    const std::lock_guard<std::mutex> lock(mu_);
    if (!stale_) {
      return std::nullopt;
    }
    stale_ = false;
    return Size{w_, h_};
  }

  /// The size, clearing the flag whatever its state. For a caller that is
  /// about to apply the size regardless -- the ring's own claim -- so the
  /// present path does not then apply it a second time in the same commit.
  ///
  /// Zero until the window has declared a mapping, which is not an error:
  /// nothing can be declared before the surface is mapped, and the next Set
  /// gives the reader its own delivery.
  Size Take() {
    const std::lock_guard<std::mutex> lock(mu_);
    stale_ = false;
    return Size{w_, h_};
  }

 private:
  std::mutex mu_;
  int32_t w_{0};
  int32_t h_{0};
  bool stale_{false};
};

}  // namespace wl_vulkan
