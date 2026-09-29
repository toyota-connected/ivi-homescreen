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

#include "cache_dir.h"

#include <cstdlib>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "logging/logger.hpp"
#include "logging/logging.h"
#include "utils.h"

namespace ihs {

namespace {

// Create a file and remove it. The only test that answers "can anything be
// written here": the mode bits can permit it while the mount refuses (EROFS),
// and a directory that already exists tells us nothing either way.
//
// O_EXCL with a pid-qualified name so two shells starting together cannot
// collide, and O_CLOEXEC so a probe that somehow outlives the call is not
// inherited across an exec.
bool CanWriteIn(const std::filesystem::path& dir) {
  const std::filesystem::path probe =
      dir / (".ihs-write-probe." + std::to_string(::getpid()));
  const int fd = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                        S_IRUSR | S_IWUSR);
  if (fd < 0) {
    return false;
  }
  ::close(fd);
  std::error_code ec;
  std::filesystem::remove(probe,
                          ec);  // best effort; we already have our answer
  return true;
}

}  // namespace

CacheDir ResolveCacheDir(const std::filesystem::path& base,
                         const std::filesystem::path& sub,
                         const std::string_view tag) {
  std::filesystem::path root = base;

  // IVI_CACHE_PATH wins over the resolved config home, but only if it is a
  // path we are willing to write to -- same rule GetConfigHomePath applies to
  // XDG_CONFIG_HOME, for the same reason.
  if (const char* env = std::getenv("IVI_CACHE_PATH");
      env != nullptr && *env != '\0') {
    if (const std::filesystem::path candidate(env);
        Utils::IsSafeBasePath(candidate)) {
      root = candidate;
    } else {
      ihs::log::warn(
          "[{}] ignoring IVI_CACHE_PATH='{}': a cache path must be absolute "
          "and contain no '..' component",
          tag, env);
    }
  }

  CacheDir out;
  out.path = sub.empty() ? root : root / sub;
  out.path = out.path.lexically_normal();

  std::error_code ec;
  const auto status = std::filesystem::status(out.path, ec);
  if (!ec && std::filesystem::exists(status) &&
      !std::filesystem::is_directory(status)) {
    // Something is there and it is not a directory. Creating it is not an
    // option and neither is using it.
    ihs::log::warn(
        "[{}] cache path '{}' exists and is not a directory; running without a "
        "persistent shader cache",
        tag, out.path.string());
    out.state = CacheDirState::kUnusable;
    return out;
  }

  if (!std::filesystem::is_directory(out.path, ec)) {
    // The error_code overload: on a read-only rootfs this reports a value
    // instead of throwing out of a constructor.
    std::filesystem::create_directories(out.path, ec);
    if (!std::filesystem::is_directory(out.path, ec)) {
      ihs::log::warn(
          "[{}] cannot create cache directory '{}' ({}); running without a "
          "persistent shader cache",
          tag, out.path.string(),
          ec ? ec.message() : std::string("unknown error"));
      out.state = CacheDirState::kUnusable;
      return out;
    }
  }

  if (CanWriteIn(out.path)) {
    out.state = CacheDirState::kWritable;
    return out;
  }

  // Present and readable but not writable: a cache warmed at build time and
  // shipped in a read-only image is the case worth keeping, so hand the engine
  // the path and let it know it may only read.
  out.state = CacheDirState::kReadOnly;
  ihs::log::warn(
      "[{}] cache directory '{}' is not writable; using it read-only. Set "
      "IVI_CACHE_PATH to a writable directory to cache shaders across runs",
      tag, out.path.string());
  return out;
}

}  // namespace ihs
