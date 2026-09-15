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
#include <memory>
#include <string>

namespace drm_kms_vulkan {

// A dma-heap that hands out physically contiguous buffers.
//
// Exists for displays whose scanout engine has no IOMMU: they can only address
// contiguous memory, so they cannot import the render GPU's exported buffer,
// which is scattered. Allocating from here instead gives one dma-buf both the
// display and the GPU can take, which is what the import-based scanout path
// presents.
//
// Only CMA-backed heaps qualify. The system heap is also a dma-heap and will
// happily allocate, but its pages are scattered and the display rejects them --
// so a name match is the selection criterion, not merely being a heap.
class ContiguousAllocator {
 public:
  // Open the best available contiguous heap. Returns nullptr with @p err set if
  // the system exposes none. IVI_DRMVK_HEAP names a heap explicitly (a bare
  // name or an absolute path) and is not second-guessed: if it cannot be
  // opened, this fails rather than quietly picking another. It names a heap,
  // not a path.
  static std::unique_ptr<ContiguousAllocator> Open(std::string& err);

  ~ContiguousAllocator();

  ContiguousAllocator(const ContiguousAllocator&) = delete;
  ContiguousAllocator& operator=(const ContiguousAllocator&) = delete;

  // Allocate @p size bytes. Returns an owned dma-buf fd, or -1 with @p err set.
  // The caller closes it.
  [[nodiscard]] int Allocate(uint64_t size, std::string& err) const;

  [[nodiscard]] const std::string& name() const noexcept { return name_; }

 private:
  ContiguousAllocator(int fd, std::string name);

  int fd_ = -1;
  std::string name_;
};

}  // namespace drm_kms_vulkan
