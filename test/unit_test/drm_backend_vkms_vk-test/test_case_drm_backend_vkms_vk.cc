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
    // No session and no shared device: the backend opens the card and takes
    // master itself, which is the --drm-no-seat arrangement. Handing it a
    // DrmDisplay's device instead would mean master was already held, and
    // Create would refuse against our own handle.
    backend_ = VulkanDrmBackend::Create(
        card_.path, /*enable_validation=*/false, /*session=*/nullptr,
        /*shared_device=*/nullptr, /*mode_spec=*/"", /*connector_name=*/"",
        /*rotation=*/0);
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

// The serial a flip event will report must already name this frame by the time
// the commit is in flight.
//
// OnFlipEvent reads c.presentation_serial to say which frame a flip showed.
// That serial used to be published after the commit returned, and every commit
// here is NONBLOCK -- so an event dispatched before the raster thread got back
// named serial-1, a frame already reported, while the frame on screen went
// unreported. The skew is permanent: each commit adds one serial and each event
// consumes one.
//
// on_commit_returned_ runs on the raster thread with the flip in flight, which
// is precisely where the event would read it. Asserting there needs no producer
// and no real flip -- the value at that instant is the whole question.
TEST_F(VulkanDrmVkms, TheSerialNamesThisFrameWhileItsCommitIsInFlight) {
  ASSERT_TRUE(PresentOneFrame()) << "the first present is the blocking modeset";

  const uint64_t before = backend_->PresentationSerialForTest();
  ASSERT_GT(before, 0u) << "no serial was ever staged, so this case cannot run";

  uint64_t seen_in_flight = 0;
  int hook_runs = 0;
  backend_->on_commit_returned_ = [this, &seen_in_flight, &hook_runs] {
    ++hook_runs;
    seen_in_flight = backend_->PresentationSerialForTest();
  };
  const bool presented = PresentOneFrame();
  backend_->on_commit_returned_ = nullptr;
  ASSERT_TRUE(presented);
  ASSERT_EQ(hook_runs, 1) << "the hook never ran, so nothing was observed";

  EXPECT_EQ(seen_in_flight, before + 1)
      << "while this frame's commit was in flight the serial still named the "
         "previous frame, so a flip event dispatched then would report the "
         "wrong "
         "one -- and every later flip would too";
}

// A VT switch must be survivable: master goes away, comes back, and the display
// has to return.
//
// This backend registered no libseat pause/resume handlers at all -- it stored
// the session and never used it -- so it kept committing into a revoked fd, its
// layer sources kept fd-bound state a resume invalidates, and nothing
// re-established the mode on the way back. The EGL backend has had this since
// drm_backend.cc:319; this is the Vulkan half.
//
// Driven directly rather than through libseat: the fixture has no seat (the
// backend self-acquires master), and what matters is the state either side of
// the callbacks, not who invoked them.
TEST_F(VulkanDrmVkms, AVtSwitchLeavesTheBackendAbleToPresent) {
  ASSERT_TRUE(PresentOneFrame()) << "the first present is the blocking modeset";
  ASSERT_TRUE(PresentOneFrame()) << "and a steady-state flip after it";
  const uint64_t before = backend_->PresentedFramesForTest();

  // Force it up rather than hoping the last flip's event has not landed yet:
  // otherwise the assertion below passes whether pause clears the latch or not.
  backend_->SetFlipPendingForTest(true);
  ASSERT_TRUE(backend_->FlipPendingForTest());

  backend_->OnSessionPaused();
  EXPECT_TRUE(backend_->SessionPaused());
  EXPECT_FALSE(backend_->FlipPendingForTest())
      << "no flip event is coming for a commit made before the revoke, so the "
         "latch it raised must not stand";

  backend_->OnSessionResumed(backend_->DrmFdForTest());
  EXPECT_FALSE(backend_->SessionPaused());
  EXPECT_FALSE(backend_->FlipPendingForTest());

  // The real assertion: frames again. Without a resume that puts first_commit
  // back, the next commit is a plain non-blocking flip against a CRTC whose
  // mode went away with master.
  EXPECT_TRUE(PresentOneFrame()) << "nothing presented after the switch back";
  EXPECT_GT(backend_->PresentedFramesForTest(), before)
      << "the backend returned true but committed nothing after resume";
}

int main(int argc, char** argv) {
  IHS_LOGGING_START("TEST", "drm_kms_vulkan vkms test");
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
