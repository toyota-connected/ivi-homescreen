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

#include <linux/sync_file.h>
#include <sys/ioctl.h>

#include <array>
#include <cstdint>

// When the sync_file @fd signaled, in CLOCK_MONOTONIC nanoseconds (the kernel
// stamps a dma_fence with ktime_get() as it signals); 0 when it has not, holds
// no fence, holds more than this reads, or cannot be read. A sync_file that
// merges several fences signals with the last of them.
[[nodiscard]] inline uint64_t SyncFileSignaledNs(const int fd) {
  if (fd < 0) {
    return 0;
  }
  std::array<sync_fence_info, 4> fences{};
  sync_file_info info{};
  info.num_fences = static_cast<__u32>(fences.size());
  info.sync_fence_info = reinterpret_cast<uintptr_t>(fences.data());
  if (::ioctl(fd, SYNC_IOC_FILE_INFO, &info) != 0 || info.status != 1 ||
      info.num_fences == 0 || info.num_fences > fences.size()) {
    return 0;
  }
  uint64_t last = 0;
  for (__u32 i = 0; i < info.num_fences; ++i) {
    if (fences[i].timestamp_ns > last) {
      last = fences[i].timestamp_ns;
    }
  }
  return last;
}
