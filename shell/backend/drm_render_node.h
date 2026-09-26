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

#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace drm_render_node {

// The render node of the DRM device behind @p dev, a DRM node's dev_t (a KMS
// card, or a render node, which is its own): the renderD* its sysfs device
// lists, as libdrm finds it, but from a dev_t rather than an open fd. 0 when
// the device has none (a display-only one, such as vc4 on a Raspberry Pi) or
// @p dev is no DRM node.
inline uint64_t Of(const uint64_t dev) {
  if (dev == 0) {
    return 0;
  }
  const std::string dir = "/sys/dev/char/" + std::to_string(major(dev)) + ":" +
                          std::to_string(minor(dev)) + "/device/drm";
  DIR* const d = ::opendir(dir.c_str());
  if (d == nullptr) {
    return 0;
  }
  uint64_t node = 0;
  while (const dirent* e = ::readdir(d)) {
    if (std::strncmp(e->d_name, "renderD", 7) != 0) {
      continue;
    }
    struct stat st{};
    const std::string path = std::string("/dev/dri/") + e->d_name;
    if (::stat(path.c_str(), &st) == 0 && S_ISCHR(st.st_mode)) {
      node = static_cast<uint64_t>(st.st_rdev);
      break;
    }
  }
  ::closedir(d);
  return node;
}

}  // namespace drm_render_node
