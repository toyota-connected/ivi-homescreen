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

#include "file_sink.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>

namespace ihs::dlt {

namespace {

// Open the log file for appending with defensive flags: O_NOFOLLOW refuses a
// symlink at the final path component (an attacker cannot redirect our append
// to an arbitrary file), O_CLOEXEC keeps the fd from leaking across exec, and
// mode 0600 keeps a freshly created log from being world-readable (logs may
// carry sensitive data). Returns nullptr on any failure; the caller then falls
// back to the console sink.
std::FILE* open_append(const std::string& path) noexcept {
  const int fd =
      ::open(path.c_str(),
             O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    return nullptr;
  }
  std::FILE* fp = ::fdopen(fd, "a");
  if (fp == nullptr) {
    ::close(fd);
    return nullptr;
  }
  return fp;
}

char level_char(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Fatal:
      return 'F';
    case LogLevel::Error:
      return 'E';
    case LogLevel::Warn:
      return 'W';
    case LogLevel::Info:
      return 'I';
    case LogLevel::Debug:
      return 'D';
    case LogLevel::Verbose:
      return 'V';
    case LogLevel::Off:
    default:
      return '?';
  }
}

std::size_t file_size(const std::string& path) noexcept {
  struct stat st{};
  if (::stat(path.c_str(), &st) == 0 && st.st_size > 0) {
    return static_cast<std::size_t>(st.st_size);
  }
  return 0;
}

}  // namespace

std::unique_ptr<RotatingFileSink> RotatingFileSink::Create(
    const std::string& path,
    std::size_t max_bytes,
    int max_files) {
  std::FILE* fp = open_append(path);
  if (fp == nullptr) {
    return nullptr;
  }
  // unique_ptr can't call the private ctor; construct via new.
  return std::unique_ptr<RotatingFileSink>(new RotatingFileSink(
      fp, path, max_bytes, max_files < 1 ? 1 : max_files, file_size(path)));
}

RotatingFileSink::RotatingFileSink(std::FILE* fp,
                                   std::string path,
                                   std::size_t max_bytes,
                                   int max_files,
                                   std::size_t initial_bytes) noexcept
    : fp_(fp),
      path_(std::move(path)),
      max_bytes_(max_bytes),
      max_files_(max_files),
      cur_bytes_(initial_bytes) {}

RotatingFileSink::~RotatingFileSink() {
  if (fp_ != nullptr) {
    std::fclose(fp_);
  }
}

void RotatingFileSink::rotate() noexcept {
  if (fp_ != nullptr) {
    std::fclose(fp_);
    fp_ = nullptr;
  }
  // Cascade: drop the oldest, shift each older file up one index, then move the
  // live file to path.1. Missing files just fail the rename harmlessly.
  const std::string oldest = path_ + "." + std::to_string(max_files_);
  std::remove(oldest.c_str());
  for (int i = max_files_ - 1; i >= 1; --i) {
    const std::string from = path_ + "." + std::to_string(i);
    const std::string to = path_ + "." + std::to_string(i + 1);
    std::rename(from.c_str(), to.c_str());
  }
  std::rename(path_.c_str(), (path_ + ".1").c_str());

  fp_ = open_append(path_);
  cur_bytes_ = 0;
}

void RotatingFileSink::write(const LogRecord& record) noexcept {
  if (fp_ == nullptr) {
    return;
  }
  const auto secs = static_cast<std::time_t>(record.ts_ns / 1'000'000'000ULL);
  const auto msec = static_cast<unsigned>((record.ts_ns / 1'000'000ULL) % 1000);
  std::tm tm{};
  ::localtime_r(&secs, &tm);
  char ts[24];
  std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03u", tm.tm_hour, tm.tm_min,
                tm.tm_sec, msec);

  const int n =
      std::fprintf(fp_, "%s [%c] %s: %s\n", ts, level_char(record.level),
                   record.tag != nullptr ? record.tag : "", record.text);
  if (n > 0) {
    cur_bytes_ += static_cast<std::size_t>(n);
    if (cur_bytes_ >= max_bytes_) {
      rotate();
    }
  }
}

void RotatingFileSink::flush() noexcept {
  if (fp_ != nullptr) {
    std::fflush(fp_);
  }
}

}  // namespace ihs::dlt
