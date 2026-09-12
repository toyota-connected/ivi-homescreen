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

// The #367 waker, driven through the real DrmDisplay against vkms.
//
// drm_flip_wake-test covers the same mechanism standalone (poll semantics plus
// an eventfd). This one calls the shipping code: DrmDisplay::DrainReadyFlips
// and DrmDisplay::WakeFlipDrain, with the card's own reader thread running.
//
// The race: WaitForPendingFlip observes flip_pending_ set, the reader consumes
// the PAGE_FLIP_EVENT, and only then does the raster thread enter the drain's
// unlocked poll. Nothing makes the fd readable again, so without a wake the
// poll runs its whole budget.
//
// Skips (never fails) without vkms + DRM master. vkms hands master to the
// first opener, so this needs no root on a host whose seat does not already
// hold the node:
//
//   sudo modprobe vkms

#include "gtest/gtest.h"

#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "display/drm_display.h"

namespace {

using Clock = std::chrono::steady_clock;

double ElapsedMs(const Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}

// Cleared by the flip handler, standing in for DrmBackend::flip_pending_.
std::atomic<bool> g_flip_pending{false};

void TestFlipHandler(int /*fd*/,
                     unsigned int /*sequence*/,
                     unsigned int /*tv_sec*/,
                     unsigned int /*tv_usec*/,
                     void* /*user_data*/) {
  g_flip_pending.store(false, std::memory_order_release);
}

// A vkms card with a mode set, driven through a real DrmDisplay.
class VkmsFlipWaker : public ::testing::Test {
 protected:
  void SetUp() override {
    for (int i = 0; i < 8 && fd_ < 0; ++i) {
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
      path_ = path;
      fd_ = fd;
    }
    if (fd_ < 0) {
      skip_ = "no vkms node found (sudo modprobe vkms)";
      return;
    }
    if (drmIsMaster(fd_) == 0 && drmSetMaster(fd_) != 0) {
      skip_ = std::string("not DRM master on ") + path_ + " (" +
              std::strerror(errno) + "); run as root";
      return;
    }
    drmModeRes* res = drmModeGetResources(fd_);
    if (res == nullptr) {
      skip_ = "drmModeGetResources failed on vkms";
      return;
    }
    for (int i = 0; i < res->count_connectors && connector_ == 0; ++i) {
      drmModeConnector* c = drmModeGetConnector(fd_, res->connectors[i]);
      if (c == nullptr) {
        continue;
      }
      if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
        connector_ = c->connector_id;
        mode_ = c->modes[0];
      }
      drmModeFreeConnector(c);
    }
    if (res->count_crtcs > 0) {
      crtc_ = res->crtcs[0];
    }
    drmModeFreeResources(res);
    if (connector_ == 0 || crtc_ == 0) {
      skip_ = "vkms exposed no connected connector + crtc";
      return;
    }
    if (!MakeFb(&fb_a_) || !MakeFb(&fb_b_)) {
      skip_ = "could not create a dumb buffer on vkms";
      return;
    }
    if (drmModeSetCrtc(fd_, crtc_, fb_a_, 0, 0, &connector_, 1, &mode_) != 0) {
      skip_ = "drmModeSetCrtc failed on vkms";
      return;
    }

    // The shipping object. AdoptFd borrows the fd (drm::Device::from_fd does
    // not close it), forces no_seat, and skips drmSetMaster -- which suits a
    // node we already mastered above.
    display_ = std::make_unique<DrmDisplay>(
        DrmDisplay::AdoptFd{}, mode_.hdisplay, mode_.vdisplay,
        static_cast<double>(mode_.vrefresh), fd_, path_,
        /*fd_owner=*/nullptr, connector_, /*revoked=*/nullptr);
    display_->SetFlipHandler(&TestFlipHandler);
    display_->StartFlipReader();
  }

  void TearDown() override {
    display_.reset();  // stops the reader thread and releases the fd
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  bool MakeFb(uint32_t* out) {
    drm_mode_create_dumb create{};
    create.width = static_cast<uint32_t>(mode_.hdisplay);
    create.height = static_cast<uint32_t>(mode_.vdisplay);
    create.bpp = 32;
    if (drmIoctl(fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
      return false;
    }
    return drmModeAddFB(fd_, static_cast<uint32_t>(mode_.hdisplay),
                        static_cast<uint32_t>(mode_.vdisplay), 24, 32,
                        create.pitch, create.handle, out) == 0;
  }

  // Queue a flip and wait for the display's reader thread to drain it, so the
  // raster side enters the drain with the event already consumed.
  bool FlipAndLetReaderDrain() {
    g_flip_pending.store(true, std::memory_order_release);
    const uint32_t fb = parity_++ % 2 == 0 ? fb_b_ : fb_a_;
    if (drmModePageFlip(fd_, crtc_, fb, DRM_MODE_PAGE_FLIP_EVENT, nullptr) !=
        0) {
      g_flip_pending.store(false, std::memory_order_release);
      return false;
    }
    const auto start = Clock::now();
    while (g_flip_pending.load(std::memory_order_acquire) &&
           ElapsedMs(start) < 2000.0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return !g_flip_pending.load(std::memory_order_acquire);
  }

  int fd_ = -1;
  uint32_t crtc_ = 0;
  uint32_t connector_ = 0;
  uint32_t fb_a_ = 0;
  uint32_t fb_b_ = 0;
  unsigned parity_ = 0;
  drmModeModeInfo mode_{};
  std::string path_;
  std::string skip_;
  std::unique_ptr<DrmDisplay> display_;
};

// The bug: with the event already drained and no wake, the real
// DrainReadyFlips runs its whole budget.
TEST_F(VkmsFlipWaker, DrainBurnsItsBudgetWithoutAWake) {
  if (!skip_.empty()) {
    GTEST_SKIP() << skip_;
  }
  ASSERT_TRUE(FlipAndLetReaderDrain()) << "vkms delivered no flip completion";

  constexpr int kBudgetMs = 60;
  const auto start = Clock::now();
  EXPECT_FALSE(display_->DrainReadyFlips(kBudgetMs));
  const double waited = ElapsedMs(start);
  EXPECT_GE(waited, kBudgetMs * 0.9)
      << "expected the drain to burn its budget; took " << waited << " ms";
}

// The fix: WakeFlipDrain, called where the backend clears flip_pending_, ends
// that wait at once.
TEST_F(VkmsFlipWaker, WakeFlipDrainEndsTheWaitImmediately) {
  if (!skip_.empty()) {
    GTEST_SKIP() << skip_;
  }
  ASSERT_TRUE(FlipAndLetReaderDrain()) << "vkms delivered no flip completion";

  // The backend signals this from every site that clears the flag; here the
  // flip is already drained, so the wake is all that is left to end the wait.
  display_->WakeFlipDrain();

  constexpr int kBudgetMs = 60;
  const auto start = Clock::now();
  EXPECT_FALSE(display_->DrainReadyFlips(kBudgetMs));
  const double waited = ElapsedMs(start);
  EXPECT_LT(waited, kBudgetMs / 2.0)
      << "the waker should have ended the wait at once; took " << waited
      << " ms";
}

}  // namespace
