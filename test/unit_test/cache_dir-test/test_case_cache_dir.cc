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

// ResolveCacheDir, the non-throwing cache-directory resolution behind #650.
//
// What this is really guarding: both call sites used to call the throwing
// overload of std::filesystem::create_directories from a constructor
// initializer list. On a read-only rootfs that raised filesystem_error with no
// handler above it, and the shell died in std::terminate having logged nothing.
// So every case here asserts a *returned value* -- reaching the assertion at
// all is half the point.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include <unistd.h>

#include "cache_dir.h"
#include "logging/logger.hpp"

namespace {

constexpr const char* kOverride = "IVI_CACHE_PATH";

class CacheDirTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (const char* prev = std::getenv(kOverride); prev != nullptr) {
      saved_override_ = prev;
    }
    ::unsetenv(kOverride);

    std::error_code ec;
    root_ = std::filesystem::temp_directory_path(ec) /
            ("ihs-cache-dir-test." + std::to_string(::getpid()));
    std::filesystem::remove_all(root_, ec);
    ASSERT_TRUE(std::filesystem::create_directories(root_, ec)) << ec.message();
  }

  void TearDown() override {
    std::error_code ec;
    // Undo any mode change first, or remove_all cannot descend.
    std::filesystem::permissions(root_, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add, ec);
    for (std::filesystem::recursive_directory_iterator it(root_, ec), end;
         !ec && it != end; ++it) {
      std::filesystem::permissions(it->path(),
                                   std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::add, ec);
    }
    ec.clear();
    std::filesystem::remove_all(root_, ec);

    if (saved_override_) {
      ::setenv(kOverride, saved_override_->c_str(), 1);
    } else {
      ::unsetenv(kOverride);
    }
  }

  /// Drop write permission on a directory. Meaningless as root, which bypasses
  /// the mode bits entirely -- a case that relies on this must skip there or it
  /// asserts the opposite of what it means.
  static void MakeUnwritable(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::permissions(p,
                                 std::filesystem::perms::owner_write |
                                     std::filesystem::perms::group_write |
                                     std::filesystem::perms::others_write,
                                 std::filesystem::perm_options::remove, ec);
    ASSERT_FALSE(ec) << ec.message();
  }

  static bool RunningAsRoot() { return ::geteuid() == 0; }

  std::filesystem::path root_;

 private:
  std::optional<std::string> saved_override_;
};

// The ordinary case: the directory does not exist yet and gets created.
TEST_F(CacheDirTest, AWritableBaseIsCreatedAndReportedWritable) {
  const auto dir = ihs::ResolveCacheDir(root_, "shaders", "test");

  EXPECT_EQ(dir.state, ihs::CacheDirState::kWritable);
  EXPECT_TRUE(dir.usable());
  EXPECT_EQ(dir.path, (root_ / "shaders").lexically_normal());
  EXPECT_TRUE(std::filesystem::is_directory(dir.path));
}

// The probe has to be a write, not an existence check. This directory exists
// and is readable; only trying to create something in it says otherwise.
TEST_F(CacheDirTest, AnUnwritableDirectoryIsReportedReadOnlyNotWritable) {
  if (RunningAsRoot()) {
    GTEST_SKIP() << "root bypasses the mode bits, so an unwritable directory "
                    "is writable for this process and the case is vacuous";
  }
  const auto target = root_ / "prewarmed";
  std::error_code ec;
  ASSERT_TRUE(std::filesystem::create_directories(target, ec)) << ec.message();
  MakeUnwritable(target);

  const auto dir = ihs::ResolveCacheDir(root_, "prewarmed", "test");

  EXPECT_EQ(dir.state, ihs::CacheDirState::kReadOnly)
      << "the directory exists and is readable, so an existence check would "
         "call it writable -- that is the bug the write probe exists for";
  EXPECT_TRUE(dir.usable()) << "a build-time-warmed cache is still worth using";
  EXPECT_EQ(dir.path, target.lexically_normal());
}

// The read-only-rootfs shape: the directory does not exist and cannot be
// created. Reaching the assertion is the test -- the old code threw here.
TEST_F(CacheDirTest, ABaseThatCannotBeCreatedInIsUnusableAndDoesNotThrow) {
  if (RunningAsRoot()) {
    GTEST_SKIP() << "root can create inside an unwritable directory";
  }
  MakeUnwritable(root_);

  ihs::CacheDir dir;
  ASSERT_NO_THROW(dir = ihs::ResolveCacheDir(root_, "shaders", "test"))
      << "this is the #650 abort: create_directories threw out of a "
         "constructor initializer list and the process terminated";

  EXPECT_EQ(dir.state, ihs::CacheDirState::kUnusable);
  EXPECT_FALSE(dir.usable());
}

// Something is at the path and it is not a directory, which is the one case the
// old code did report -- keep reporting it, without exiting.
TEST_F(CacheDirTest, AFileWhereTheDirectoryShouldBeIsUnusable) {
  const auto target = root_ / "shaders";
  {
    std::ofstream(target) << "not a directory";
  }
  ASSERT_TRUE(std::filesystem::is_regular_file(target));

  const auto dir = ihs::ResolveCacheDir(root_, "shaders", "test");

  EXPECT_EQ(dir.state, ihs::CacheDirState::kUnusable);
  EXPECT_FALSE(dir.usable());
}

// The override exists so an integrator can point the cache at a tmpfs or a
// writable partition instead of depending on the session's XDG_CONFIG_HOME.
TEST_F(CacheDirTest, TheOverrideReplacesTheBase) {
  const auto elsewhere = root_ / "elsewhere";
  std::error_code ec;
  ASSERT_TRUE(std::filesystem::create_directories(elsewhere, ec))
      << ec.message();
  ::setenv(kOverride, elsewhere.c_str(), 1);

  const auto dir = ihs::ResolveCacheDir(root_ / "ignored", "shaders", "test");

  EXPECT_EQ(dir.state, ihs::CacheDirState::kWritable);
  EXPECT_EQ(dir.path, (elsewhere / "shaders").lexically_normal());
}

// The override is written to, so an untrusted value must not redirect it. Same
// rule GetConfigHomePath applies to XDG_CONFIG_HOME.
TEST_F(CacheDirTest, AnUnsafeOverrideIsIgnoredRatherThanFollowed) {
  ::setenv(kOverride, "relative/not/absolute", 1);
  auto dir = ihs::ResolveCacheDir(root_, "shaders", "test");
  EXPECT_EQ(dir.path, (root_ / "shaders").lexically_normal())
      << "a relative override was followed";

  const std::string traversal = (root_ / ".." / "escaped").string();
  ::setenv(kOverride, traversal.c_str(), 1);
  dir = ihs::ResolveCacheDir(root_, "shaders", "test");
  EXPECT_EQ(dir.path, (root_ / "shaders").lexically_normal())
      << "an override containing '..' was followed out of the config tree";
}

// An empty override is not an override.
TEST_F(CacheDirTest, AnEmptyOverrideLeavesTheBaseAlone) {
  ::setenv(kOverride, "", 1);
  const auto dir = ihs::ResolveCacheDir(root_, "shaders", "test");
  EXPECT_EQ(dir.path, (root_ / "shaders").lexically_normal());
  EXPECT_EQ(dir.state, ihs::CacheDirState::kWritable);
}

// An empty subdirectory means the base itself, which is what Engine passes.
TEST_F(CacheDirTest, AnEmptySubResolvesToTheBase) {
  const auto dir = ihs::ResolveCacheDir(root_, {}, "test");
  EXPECT_EQ(dir.path, root_.lexically_normal());
  EXPECT_EQ(dir.state, ihs::CacheDirState::kWritable);
}

// The probe must not leave anything behind: the cache directory is handed to
// the engine, and a stray file in it is our litter.
TEST_F(CacheDirTest, TheWriteProbeRemovesItself) {
  const auto dir = ihs::ResolveCacheDir(root_, "shaders", "test");
  ASSERT_EQ(dir.state, ihs::CacheDirState::kWritable);

  size_t entries = 0;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(dir.path, ec), end;
       !ec && it != end; ++it) {
    ++entries;
  }
  EXPECT_EQ(entries, 0u) << "the write probe left a file in the cache dir";
}

}  // namespace

int main(int argc, char** argv) {
  IHS_LOGGING_START("TEST", "cache_dir test");
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
