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

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <xf86drm.h>

#include "backend/drm_render_node.h"

namespace {

uint64_t DevOf(const std::string& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_rdev) : 0;
}

std::vector<std::string> Nodes(const char* prefix) {
  std::vector<std::string> out;
  DIR* const d = ::opendir("/dev/dri");
  if (d == nullptr) {
    return out;
  }
  while (const dirent* e = ::readdir(d)) {
    if (std::strncmp(e->d_name, prefix, std::strlen(prefix)) == 0) {
      out.push_back(std::string("/dev/dri/") + e->d_name);
    }
  }
  ::closedir(d);
  return out;
}

// libdrm's render node for the card at @p path, or 0 when it names none.
uint64_t LibdrmRenderNode(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return 0;
  }
  char* const name = drmGetRenderDeviceNameFromFd(fd);
  ::close(fd);
  if (name == nullptr) {
    return 0;
  }
  const uint64_t dev = DevOf(name);
  std::free(name);
  return dev;
}

}  // namespace

// Each card maps to the render node libdrm finds for it, or to none alike.
TEST(DrmRenderNode, MatchesLibdrmForEveryCard) {
  const std::vector<std::string> cards = Nodes("card");
  if (cards.empty()) {
    GTEST_SKIP() << "no DRM card";
  }
  for (const std::string& card : cards) {
    SCOPED_TRACE(card);
    EXPECT_EQ(drm_render_node::Of(DevOf(card)), LibdrmRenderNode(card));
  }
}

// A render node is its own.
TEST(DrmRenderNode, ARenderNodeIsItsOwn) {
  const std::vector<std::string> nodes = Nodes("renderD");
  if (nodes.empty()) {
    GTEST_SKIP() << "no render node";
  }
  for (const std::string& node : nodes) {
    SCOPED_TRACE(node);
    EXPECT_EQ(drm_render_node::Of(DevOf(node)), DevOf(node));
  }
}

// Nothing for no device, or for one that is not DRM.
TEST(DrmRenderNode, NoneForNonDrm) {
  EXPECT_EQ(drm_render_node::Of(0), 0U);
  EXPECT_EQ(drm_render_node::Of(DevOf("/dev/null")), 0U);
}
