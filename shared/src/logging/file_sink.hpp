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

#ifndef IHS_SHARED_SRC_LOGGING_FILE_SINK_HPP_
#define IHS_SHARED_SRC_LOGGING_FILE_SINK_HPP_

#include "log_sink.hpp"

#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>

namespace ihs::dlt {

// Size-rotating file sink. Writes "ts [L] tag: text" lines to a path; when the
// active file passes max_bytes it renames path -> path.1 (cascading path.N-1
// -> path.N up to max_files) and reopens a fresh path. Rotation runs on the
// single drain worker thread, so it needs no locking and cannot race a writer.
// No third-party dependency: the rename cascade is hand-rolled.
class RotatingFileSink final : public Sink {
 public:
  // Returns null if the file cannot be opened (caller warns + falls back).
  static std::unique_ptr<RotatingFileSink> Create(const std::string& path,
                                                  std::size_t max_bytes,
                                                  int max_files);
  ~RotatingFileSink() override;

  RotatingFileSink(const RotatingFileSink&) = delete;
  RotatingFileSink& operator=(const RotatingFileSink&) = delete;

  void write(const LogRecord& record) noexcept override;
  void flush() noexcept override;

 private:
  RotatingFileSink(std::FILE* fp,
                   std::string path,
                   std::size_t max_bytes,
                   int max_files,
                   std::size_t initial_bytes) noexcept;
  void rotate() noexcept;

  std::FILE* fp_;
  std::string path_;
  std::size_t max_bytes_;
  int max_files_;
  std::size_t cur_bytes_;
};

}  // namespace ihs::dlt

#endif  // IHS_SHARED_SRC_LOGGING_FILE_SINK_HPP_
