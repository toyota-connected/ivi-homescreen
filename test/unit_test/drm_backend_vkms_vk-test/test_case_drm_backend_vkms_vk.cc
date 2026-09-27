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

// A real VulkanDrmBackend on a vkms card, driven the way the engine drives it.
//
// The backend had no test at all before this: it refuses a CPU/software Vulkan
// device, and a CI runner has no GPU, so nothing could bring it up.
// IVI_DRMVK_ALLOW_SOFTWARE=1 lifts that refusal, and llvmpipe then does real
// zero-copy scanout on vkms -- dma-buf exported from the ICD, imported as a KMS
// framebuffer, flipped on a plane.
//
// The fixture stands in for the engine: it takes a backing store through the
// compositor callback, draws nothing into it, and presents it. That is enough
// to drive a real commit and a real page flip, which is what every ordering
// question about this backend needs.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "backend/drm_kms_vulkan/vulkan_drm_backend.h"
#include "logging/logger.hpp"

extern "C" {
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
}

namespace {

constexpr const char* kAllowSoftware = "IVI_DRMVK_ALLOW_SOFTWARE";

struct VkmsCard {
  std::string path;
  uint32_t mode_w{0};
  uint32_t mode_h{0};
  int connectors{0};
  int connected{0};

  [[nodiscard]] bool ok() const { return !path.empty() && mode_w != 0; }
};

// Same discovery as the EGL vkms fixture: match drmGetVersion, then take the
// first connected connector that has a mode. Duplicated rather than shared
// because the two fixtures are separate binaries with no common header.
VkmsCard FindVkms() {
  VkmsCard out;
  for (int i = 0; i < 8; ++i) {
    const std::string path = "/dev/dri/card" + std::to_string(i);
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    drmVersionPtr v = drmGetVersion(fd);
    const bool is_vkms =
        v != nullptr && v->name != nullptr && std::string(v->name) == "vkms";
    if (v != nullptr) {
      drmFreeVersion(v);
    }
    if (!is_vkms) {
      ::close(fd);
      continue;
    }
    out.path = path;
    if (drmModeRes* res = drmModeGetResources(fd); res != nullptr) {
      for (int c = 0; c < res->count_connectors; ++c) {
        drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[c]);
        if (conn == nullptr) {
          continue;
        }
        out.connectors++;
        if (conn->connection == DRM_MODE_CONNECTED) {
          out.connected++;
          if (out.mode_w == 0 && conn->count_modes > 0) {
            out.mode_w = conn->modes[0].hdisplay;
            out.mode_h = conn->modes[0].vdisplay;
          }
        }
        drmModeFreeConnector(conn);
      }
      drmModeFreeResources(res);
    }
    ::close(fd);
    if (out.ok()) {
      break;
    }
  }
  return out;
}

class VulkanDrmVkms : public ::testing::Test {
 protected:
  void SetUp() override {
    card_ = FindVkms();
    if (card_.path.empty()) {
      GTEST_SKIP() << "no vkms card on this host (sudo modprobe vkms)";
    }
    if (!card_.ok()) {
      GTEST_SKIP() << card_.path << " is vkms but exposes no connected "
                   << "connector with modes (connectors=" << card_.connectors
                   << " connected=" << card_.connected << ")";
    }

    // Before Create: device selection reads this, and without it the backend
    // refuses every CPU device -- which on a runner is the only kind there is.
    if (const char* prev = std::getenv(kAllowSoftware); prev != nullptr) {
      saved_allow_ = prev;
    }
    ::setenv(kAllowSoftware, "1", 1);

    // No DrmDisplay here, deliberately. This backend opens the card and
    // acquires DRM master itself; a DrmDisplay would take master first and
    // Create would then refuse with EACCES against our own handle -- which
    // reads exactly like "another display server holds this card".
    backend_ = VulkanDrmBackend::Create(
        card_.path, /*enable_validation=*/false, /*session=*/nullptr,
        /*mode_spec=*/"", /*connector_name=*/"", /*rotation=*/0);
    if (backend_ == nullptr) {
      // No Vulkan loader, or no device that can import dma-buf at all. A skip,
      // not a failure: the host simply cannot run this backend.
      GTEST_SKIP() << "VulkanDrmBackend::Create refused on " << card_.path
                   << " (no usable Vulkan device for zero-copy scanout)";
    }
  }

  void TearDown() override {
    backend_.reset();
    if (saved_allow_) {
      ::setenv(kAllowSoftware, saved_allow_->c_str(), 1);
    } else {
      ::unsetenv(kAllowSoftware);
    }
  }

  // One frame: take a store, present it as the single layer. The engine would
  // render into it first; nothing here needs its contents, only that a real
  // buffer reaches a real commit.
  bool PresentOneFrame() {
    FlutterBackingStoreConfig cfg{};
    cfg.struct_size = sizeof(FlutterBackingStoreConfig);
    cfg.size = FlutterSize{static_cast<double>(card_.mode_w),
                           static_cast<double>(card_.mode_h)};
    FlutterBackingStore bs{};
    if (!backend_->CreateBackingStoreForTest(&cfg, &bs)) {
      return false;
    }

    FlutterLayer layer{};
    layer.struct_size = sizeof(FlutterLayer);
    layer.type = kFlutterLayerContentTypeBackingStore;
    layer.backing_store = &bs;
    layer.offset = FlutterPoint{0.0, 0.0};
    layer.size = cfg.size;
    const FlutterLayer* layers[] = {&layer};

    const bool ok = backend_->PresentLayersForTest(layers, 1);
    backend_->CollectBackingStoreForTest(&bs);
    return ok;
  }

  VkmsCard card_;
  std::shared_ptr<VulkanDrmBackend> backend_;

 private:
  std::optional<std::string> saved_allow_;
};

}  // namespace

// The bring-up this fixture exists to make possible. Before
// IVI_DRMVK_ALLOW_SOFTWARE there was no way to reach this line without a GPU.
TEST_F(VulkanDrmVkms, TheBackendComesUpOnVkms) {
  ASSERT_NE(backend_, nullptr);
  EXPECT_GT(backend_->width(), 0u);
  EXPECT_GT(backend_->height(), 0u);
}

// A real commit and a real flip, which every ordering question about this
// backend needs -- the presentation serial (#662) cannot be asserted from a
// seam, only from a frame that actually reached the display.
TEST_F(VulkanDrmVkms, APresentedFrameReachesTheDisplay) {
  ASSERT_TRUE(PresentOneFrame()) << "the first present is the blocking modeset";
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(PresentOneFrame()) << "frame " << i << " was not presented";
  }
  // Not the return value alone: the backend's own commit count, so a present
  // that succeeded without reaching a commit cannot pass this.
  EXPECT_GE(backend_->PresentedFramesForTest(), 5u)
      << "five presents returned true but the backend committed fewer frames";
}

int main(int argc, char** argv) {
  IHS_LOGGING_START("TEST", "drm_kms_vulkan vkms test");
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
