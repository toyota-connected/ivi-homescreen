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

#include <cstring>
#include <mutex>
#include <vector>

#include "backend/software/surface_sink.h"

// In-process snapshot store. Each Present() copies the buffer; tests
// read the latest snapshot via SnapshotLatest(). Mutex because the
// rasterizer-thread writer races with the test-thread reader.
class MemorySink final : public ISurfaceSink {
 public:
  MemorySink() = default;

  bool Present(const void* allocation,
               size_t row_bytes,
               size_t height) override {
    std::lock_guard<std::mutex> lock(mu_);
    const size_t total = row_bytes * height;
    if (latest_.size() != total) {
      latest_.resize(total);
    }
    std::memcpy(latest_.data(), allocation, total);
    latest_row_bytes_ = row_bytes;
    latest_height_ = height;
    ++frame_count_;
    return true;
  }

  // Test API: returns a copy of the most recent buffer. Out-params
  // receive row_bytes and height; both are 0 if no frame has presented.
  std::vector<uint8_t> SnapshotLatest(size_t* row_bytes, size_t* height) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (row_bytes != nullptr) {
      *row_bytes = latest_row_bytes_;
    }
    if (height != nullptr) {
      *height = latest_height_;
    }
    return latest_;
  }

  [[nodiscard]] uint64_t frame_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return frame_count_;
  }

 private:
  mutable std::mutex mu_;
  std::vector<uint8_t> latest_;
  size_t latest_row_bytes_{0};
  size_t latest_height_{0};
  uint64_t frame_count_{0};
};
