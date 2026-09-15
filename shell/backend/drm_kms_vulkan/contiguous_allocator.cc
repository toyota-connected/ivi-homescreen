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

#include "contiguous_allocator.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#if __has_include(<linux/dma-heap.h>)
#include <linux/dma-heap.h>
#else
// dma-heap landed in 5.6; the uapi is stable, so a sysroot too old to carry the
// header does not have to mean a build without the contiguous path.
#include <linux/ioctl.h>
#include <linux/types.h>
struct dma_heap_allocation_data {
  __u64 len;
  __u32 fd;
  __u32 fd_flags;
  __u64 heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)
#endif

namespace drm_kms_vulkan {

namespace {

constexpr const char* kHeapDir = "/dev/dma_heap";

// Heaps whose pages are physically contiguous, best first. The kernel names
// these after the device-tree reserved-memory node, so the spelling varies:
// "linux,cma" is the generic one, "reserved" and "default_cma_region" are what
// some platforms call theirs. Matching "cma" as a substring catches vendor
// prefixes ("vendor,cma") without admitting the system heap.
bool IsContiguousHeapName(const std::string& name) {
  if (name.find("cma") != std::string::npos) {
    return true;
  }
  return name == "reserved";
}

// Prefer the generic name, then anything else CMA-ish. Deterministic, so two
// boots of the same board pick the same heap.
int HeapRank(const std::string& name) {
  if (name == "linux,cma") {
    return 0;
  }
  if (name.find("cma") != std::string::npos) {
    return 1;
  }
  return 2;
}

}  // namespace

ContiguousAllocator::ContiguousAllocator(const int fd, std::string name)
    : fd_(fd), name_(std::move(name)) {}

ContiguousAllocator::~ContiguousAllocator() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

std::unique_ptr<ContiguousAllocator> ContiguousAllocator::Open(
    std::string& err) {
  if (const char* forced = std::getenv("IVI_DRMVK_HEAP");
      forced != nullptr && *forced != '\0') {
    const std::string spec(forced);
    // A heap name, never a path: this names one of the heaps the kernel
    // exposes, so there is no reason for it to reach outside that directory and
    // every reason not to let an environment variable pick an arbitrary device
    // to open read-write.
    if (spec.find('/') != std::string::npos) {
      err =
          "IVI_DRMVK_HEAP must be a heap name, not a path (got '" + spec + "')";
      return nullptr;
    }
    const std::string path = std::string(kHeapDir) + "/" + spec;
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      err = "IVI_DRMVK_HEAP=" + spec + ": " + std::strerror(errno);
      return nullptr;
    }
    return std::unique_ptr<ContiguousAllocator>(
        new ContiguousAllocator(fd, spec));
  }

  std::error_code ec;
  std::vector<std::string> candidates;
  for (const auto& entry : std::filesystem::directory_iterator(kHeapDir, ec)) {
    if (std::string name = entry.path().filename().string();
        IsContiguousHeapName(name)) {
      candidates.push_back(std::move(name));
    }
  }
  if (ec) {
    err = std::string(kHeapDir) + ": " + ec.message();
    return nullptr;
  }
  if (candidates.empty()) {
    err = "no contiguous (CMA) dma-heap under " + std::string(kHeapDir);
    return nullptr;
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const std::string& a, const std::string& b) {
              const int ra = HeapRank(a);
              const int rb = HeapRank(b);
              return ra != rb ? ra < rb : a < b;
            });

  std::string last_error;
  for (const auto& name : candidates) {
    const std::string path = std::string(kHeapDir) + "/" + name;
    if (const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC); fd >= 0) {
      return std::unique_ptr<ContiguousAllocator>(
          new ContiguousAllocator(fd, name));
    }
    last_error = name + ": " + std::strerror(errno);
  }
  err = "no contiguous dma-heap could be opened (" + last_error + ")";
  return nullptr;
}

int ContiguousAllocator::Allocate(const uint64_t size, std::string& err) const {
  dma_heap_allocation_data data{};
  data.len = size;
  data.fd_flags = O_RDWR | O_CLOEXEC;
  if (::ioctl(fd_, DMA_HEAP_IOCTL_ALLOC, &data) != 0) {
    err = "dma-heap " + name_ + " allocation of " + std::to_string(size) +
          " bytes failed: " + std::strerror(errno);
    return -1;
  }
  return static_cast<int>(data.fd);
}

}  // namespace drm_kms_vulkan
