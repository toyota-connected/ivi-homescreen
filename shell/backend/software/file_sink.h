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
#include <string>

#include "backend/software/surface_sink.h"

// Writes each frame to disk as a NetPBM PAM (P7) file with RGBA tuple
// type — no third-party encoder dependency, RGBA8888 preserved
// (matching Flutter's surface_present_callback format), and viewable
// by ImageMagick / GIMP / feh / many others.
//
// Path pattern: any printf-style "%d" / "%05d" / etc. is replaced with
// the frame index. A pattern without any "%" is treated as a single-
// shot path — only the first frame is written, subsequent presents
// return true without touching disk.
class FileSink final : public ISurfaceSink {
 public:
  // @p path_pattern is a printf-style format string accepting one
  // integer (e.g. "out/frame_%05d.pam"). Pass a literal path with no
  // "%" to capture only the first frame.
  explicit FileSink(std::string path_pattern);

  bool Present(const void* allocation,
               size_t row_bytes,
               size_t height) override;

 private:
  // PAM header + raw RGBA8888 pixel rows. Returns true on success.
  static bool WritePam(const std::string& path,
                       const void* allocation,
                       size_t row_bytes,
                       size_t height);

  std::string path_pattern_;
  bool single_shot_;
  bool single_shot_written_{false};
  uint64_t frame_index_{0};
};
