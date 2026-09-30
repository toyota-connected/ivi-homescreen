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

#include <filesystem>
#include <string_view>

namespace ihs {

/// What a resolved cache directory can be used for.
enum class CacheDirState {
  /// Present and we can create files in it.
  kWritable,
  /// Present and readable, but not writable. Usable for a cache warmed at
  /// build time and shipped read-only in the image; the engine is told so
  /// through is_persistent_cache_read_only.
  kReadOnly,
  /// Neither present nor creatable, or present and not a directory. The engine
  /// gets no cache path at all.
  kUnusable,
};

struct CacheDir {
  std::filesystem::path path;
  CacheDirState state{CacheDirState::kUnusable};

  [[nodiscard]] bool usable() const {
    return state != CacheDirState::kUnusable;
  }
};

/**
 * @brief Resolve a cache directory under @p base, reporting what it is good for
 *
 * A shader cache is an optimization, so nothing here is fatal and nothing
 * throws. Both call sites previously used the throwing overload of
 * create_directories: on a read-only rootfs that raised filesystem_error out of
 * a constructor initializer list, with no handler above it, and the process
 * died in std::terminate rather than reporting anything (#650).
 *
 * @param base    directory to resolve under, normally
 * Utils::GetConfigHomePath()
 * @param sub     optional subdirectory of @p base; empty means @p base itself
 * @param tag     log prefix for the one warning this may emit
 *
 * IVI_CACHE_PATH overrides @p base outright, so an integrator can point the
 * cache at a tmpfs or a writable partition instead of depending on whatever
 * XDG_CONFIG_HOME happens to be in the session. It is validated the way
 * XDG_CONFIG_HOME is (absolute, no ".." component) and ignored otherwise --
 * this path is written to, so an untrusted value must not redirect it.
 *
 * Writability is probed by creating a file and removing it. A directory that
 * exists, or a create_directories that returns true, says nothing about whether
 * anything can be written inside it: the mount may be read-only, or the mode
 * may not permit it.
 */
CacheDir ResolveCacheDir(const std::filesystem::path& base,
                         const std::filesystem::path& sub,
                         std::string_view tag);

}  // namespace ihs
