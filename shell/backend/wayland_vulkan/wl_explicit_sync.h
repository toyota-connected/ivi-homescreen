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

#ifndef SHELL_BACKEND_WAYLAND_VULKAN_WL_EXPLICIT_SYNC_H_
#define SHELL_BACKEND_WAYLAND_VULKAN_WL_EXPLICIT_SYNC_H_

#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

struct wl_display;
struct wl_proxy;

namespace wl {
class CRegistry;
}

namespace wl_vulkan {

// Explicit sync for the dma-buf present path via wp_linux_drm_syncobj.
//
// Instead of a CPU fence (vkWaitForFences after the blit, stalling the
// rasterizer until the GPU finishes so the compositor never samples a partial
// frame), the compositor is handed timeline sync points:
//   * acquire — the blit submit signals a timeline VkSemaphore imported FROM a
//     DRM syncobj; the compositor waits on that point before sampling.
//   * release — the compositor signals a second DRM syncobj when done; the slot
//     is reclaimed by polling it (the explicit-sync analogue of
//     wl_buffer.release), so no CPU stall and no cross-thread release event.
//
// All the DRM-syncobj + Vulkan-import plumbing is Vulkan/libdrm specific and
// lives here; the generated wp_linux_drm_syncobj protocol types are hidden
// behind a PIMPL so this header stays free of them.
class ExplicitSync {
 public:
  // A frame's acquire (GPU-signalled) and release (compositor-signalled)
  // timeline points. Both are monotonic; release is recorded per slot.
  struct FramePoints {
    uint64_t acquire = 0;
    uint64_t release = 0;
  };

  // Bring up the drm_syncobj <-> Vulkan-timeline bridge and the syncobj surface
  // for @p surface. Returns nullptr (the caller keeps the CPU-fence path) if
  // @p manager_name is 0, the render node can't be resolved/opened, or the
  // device lacks timeline-semaphore / external-semaphore-fd / OPAQUE_FD import.
  // @p err carries the reason on failure.
  static std::unique_ptr<ExplicitSync> Create(VkPhysicalDevice physical_device,
                                              VkDevice device,
                                              wl::CRegistry& registry,
                                              uint32_t manager_name,
                                              uint32_t manager_version,
                                              wl_display* display,
                                              wl_proxy* surface,
                                              std::string& err);
  ~ExplicitSync();
  ExplicitSync(const ExplicitSync&) = delete;
  ExplicitSync& operator=(const ExplicitSync&) = delete;

  // The timeline semaphore the blit submit must signal at FramePoints::acquire
  // (chain a VkTimelineSemaphoreSubmitInfo with that signal value).
  [[nodiscard]] VkSemaphore acquire_semaphore() const;

  // Allocate this frame's acquire + release points. Call once per present,
  // before the blit submit.
  [[nodiscard]] FramePoints NextFrame();

  // Attach the frame's acquire + release points to the syncobj surface. MUST be
  // called between wl_surface.attach and wl_surface.commit.
  void SetSurfacePoints(const FramePoints& points);

  // Non-blocking: has the compositor signalled the release syncobj at
  // @p release_point yet? Replaces wl_buffer.release for slot reclaim.
  // @p release_point == 0 (slot never committed) returns true.
  [[nodiscard]] bool SlotReleased(uint64_t release_point) const;

 private:
  ExplicitSync();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace wl_vulkan

#endif  // SHELL_BACKEND_WAYLAND_VULKAN_WL_EXPLICIT_SYNC_H_
