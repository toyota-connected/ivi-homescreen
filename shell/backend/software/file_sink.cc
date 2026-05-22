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

#include "backend/software/file_sink.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <utility>

#include "logging.h"

FileSink::FileSink(std::string path_pattern)
    : path_pattern_(std::move(path_pattern)),
      single_shot_(path_pattern_.find('%') == std::string::npos) {}

bool FileSink::Present(const void* allocation,
                       const size_t row_bytes,
                       const size_t height) {
  if (single_shot_) {
    if (single_shot_written_) {
      return true;
    }
    if (!WritePam(path_pattern_, allocation, row_bytes, height)) {
      return false;
    }
    single_shot_written_ = true;
    return true;
  }

  // Multi-shot — interpolate the frame index into the pattern. The
  // pattern is operator-supplied so cap the formatted length
  // generously (PATH_MAX is the eventual ceiling). snprintf truncates
  // safely if the operator typed something unreasonable.
  std::array<char, 4096> buf{};
  const int written =
      std::snprintf(buf.data(), buf.size(), path_pattern_.c_str(),
                    static_cast<int>(frame_index_));
  if (written < 0 || static_cast<size_t>(written) >= buf.size()) {
    spdlog::error(
        "[FileSink] formatted path overflowed or failed for pattern '{}'",
        path_pattern_);
    return false;
  }
  const bool ok = WritePam(buf.data(), allocation, row_bytes, height);
  ++frame_index_;
  return ok;
}

bool FileSink::WritePam(const std::string& path,
                        const void* allocation,
                        const size_t row_bytes,
                        const size_t height) {
  // Ensure the parent directory exists so the operator doesn't have to
  // mkdir manually. create_directories is a no-op if the directory
  // already exists.
  std::error_code ec;
  const auto parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    // create_directories returning false isn't fatal — the directory
    // may already exist. The fopen below will surface a real error.
  }

  FILE* fp = std::fopen(path.c_str(), "wb");
  if (fp == nullptr) {
    spdlog::error("[FileSink] cannot open '{}' for writing", path);
    return false;
  }

  // row_bytes is the stride. PAM expects packed RGBA8888 with 4 bytes
  // per pixel, no padding. width = row_bytes / 4.
  const size_t width = row_bytes / 4;
  std::fprintf(fp,
               "P7\n"
               "WIDTH %zu\n"
               "HEIGHT %zu\n"
               "DEPTH 4\n"
               "MAXVAL 255\n"
               "TUPLTYPE RGB_ALPHA\n"
               "ENDHDR\n",
               width, height);

  // If row_bytes is exactly width*4, one write suffices. If the
  // engine ever produces a strided buffer, write each row's leading
  // width*4 bytes and skip the padding.
  const auto* bytes = static_cast<const uint8_t*>(allocation);
  const size_t row_pixels_bytes = width * 4;
  bool ok = true;
  if (row_bytes == row_pixels_bytes) {
    if (std::fwrite(bytes, 1, row_bytes * height, fp) != row_bytes * height) {
      ok = false;
    }
  } else {
    for (size_t y = 0; y < height; ++y) {
      if (std::fwrite(bytes + y * row_bytes, 1, row_pixels_bytes, fp) !=
          row_pixels_bytes) {
        ok = false;
        break;
      }
    }
  }

  if (!ok) {
    spdlog::error("[FileSink] short write to '{}'", path);
  }
  std::fclose(fp);
  return ok;
}
