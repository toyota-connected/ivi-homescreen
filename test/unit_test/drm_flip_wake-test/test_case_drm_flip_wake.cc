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

// The PAGE_FLIP_EVENT drain race behind #367, against a real DRM device.
//
// DrmBackend::WaitForPendingFlip loads flip_pending_, sees it set, and then
// calls DrmDisplay::DrainReadyFlips(remaining), which blocks in an UNLOCKED
// poll() on the card fd. If the card's flip reader thread consumes the event
// in the window between that load and the poll, nothing will ever make the fd
// readable again and the poll runs its whole budget -- ~100 ms, silently,
// because the loop then exits on the flag rather than on the deadline.
//
// These cases drive the same structure against vkms: a real page flip, a
// reader thread draining it, and the raster-side poll entered after the drain.
// They assert the mechanism rather than the shell's current behavior, so they
// hold both before and after a fix lands:
//
//   * with nothing in the poll set but the card fd, the poll burns its budget
//   * with an eventfd in the poll set, signalled where the flag is cleared,
//     the poll returns at once
//
// Everything skips (not fails) without vkms and DRM master, so this is safe to
// build and run unprivileged and in CI. vkms usually grants master implicitly
// to the first opener; otherwise run under sudo.
//
//   sudo modprobe vkms

#include "gtest/gtest.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

double ElapsedMs(const Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}

// A vkms card, mastered, with one connected connector and a scanout buffer.
// Everything is skipped rather than failed when the environment cannot supply
// it, so the case is safe everywhere.
class VkmsFlip : public ::testing::Test {
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
      fd_ = fd;
    }
    if (fd_ < 0) {
      skip_ = "no vkms node found (sudo modprobe vkms)";
      return;
    }
    if (drmIsMaster(fd_) == 0 && drmSetMaster(fd_) != 0) {
      skip_ = "not DRM master on the vkms node; run under sudo";
      return;
    }
    drmModeRes* res = drmModeGetResources(fd_);
    if (res == nullptr) {
      skip_ = "drmModeGetResources failed on vkms";
      return;
    }
    for (int i = 0; i < res->count_connectors && conn_ == nullptr; ++i) {
      drmModeConnector* c = drmModeGetConnector(fd_, res->connectors[i]);
      if (c == nullptr) {
        continue;
      }
      if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
        conn_ = c;
      } else {
        drmModeFreeConnector(c);
      }
    }
    if (conn_ == nullptr || res->count_crtcs == 0) {
      drmModeFreeResources(res);
      skip_ = "vkms exposed no connected connector + crtc";
      return;
    }
    crtc_ = res->crtcs[0];
    mode_ = conn_->modes[0];
    drmModeFreeResources(res);

    if (!MakeFb(&fb_a_) || !MakeFb(&fb_b_)) {
      skip_ = "could not create a dumb buffer on vkms";
      return;
    }
    if (drmModeSetCrtc(fd_, crtc_, fb_a_, 0, 0, &conn_->connector_id, 1,
                       &mode_) != 0) {
      skip_ = "drmModeSetCrtc failed on vkms";
      return;
    }
    wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd_ < 0) {
      skip_ = "eventfd unavailable";
    }
  }

  void TearDown() override {
    reader_run_.store(false);
    if (reader_.joinable()) {
      reader_.join();
    }
    if (wake_fd_ >= 0) {
      ::close(wake_fd_);
    }
    if (conn_ != nullptr) {
      drmModeFreeConnector(conn_);
    }
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  bool MakeFb(uint32_t* out) {
    drm_mode_create_dumb create{};
    create.width = mode_.hdisplay;
    create.height = mode_.vdisplay;
    create.bpp = 32;
    if (drmIoctl(fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
      return false;
    }
    return drmModeAddFB(fd_, static_cast<uint32_t>(mode_.hdisplay),
                        static_cast<uint32_t>(mode_.vdisplay), 24, 32,
                        create.pitch, create.handle, out) == 0;
  }

  // The handler clears the flag, as UnifiedPageFlipHandler does. With a waker
  // installed it also signals it -- the shape the fix would take.
  static void OnFlip(int /*fd*/,
                     unsigned int /*seq*/,
                     unsigned int /*sec*/,
                     unsigned int /*usec*/,
                     void* data) {
    auto* self = static_cast<VkmsFlip*>(data);
    self->flip_pending_.store(false);
    if (self->waker_armed_ && self->wake_fd_ >= 0) {
      const uint64_t one = 1;
      const ssize_t n = ::write(self->wake_fd_, &one, sizeof(one));
      static_cast<void>(n);
    }
  }

  // DrmDisplay::DrainReadyFlips: unlocked poll, then the serialized read.
  // @extra_fd mirrors adding a waker to that poll set.
  bool Drain(int timeout_ms, int extra_fd) {
    pollfd pfds[2];
    pfds[0] = pollfd{fd_, POLLIN, 0};
    pfds[1] = pollfd{extra_fd, POLLIN, 0};
    const nfds_t count = extra_fd >= 0 ? 2 : 1;
    if (::poll(pfds, count, timeout_ms) <= 0) {
      return false;
    }
    if (count == 2 && (pfds[1].revents & POLLIN) != 0) {
      uint64_t drained = 0;
      const ssize_t n = ::read(extra_fd, &drained, sizeof(drained));
      static_cast<void>(n);
      return false;  // the caller re-checks the flag, as the wait loop does
    }
    if ((pfds[0].revents & POLLIN) == 0) {
      return false;
    }
    const std::lock_guard<std::mutex> lock(drain_mu_);
    pollfd again{fd_, POLLIN, 0};
    if (::poll(&again, 1, 0) <= 0 || (again.revents & POLLIN) == 0) {
      return false;  // the reader consumed it; no double read
    }
    drmEventContext ctx{};
    ctx.version = 2;
    ctx.page_flip_handler = &VkmsFlip::OnFlip;
    drmHandleEvent(fd_, &ctx);
    return true;
  }

  void StartReader() {
    reader_run_.store(true);
    reader_ = std::thread([this] {
      while (reader_run_.load()) {
        Drain(50, -1);
      }
    });
  }

  // Queue a flip and wait until the reader has taken it, so the raster side
  // enters its poll with the event already consumed -- the #367 interleaving.
  bool FlipAndLetReaderDrain() {
    flip_pending_.store(true);
    const uint32_t fb = flip_parity_++ % 2 == 0 ? fb_b_ : fb_a_;
    if (drmModePageFlip(fd_, crtc_, fb, DRM_MODE_PAGE_FLIP_EVENT, this) != 0) {
      flip_pending_.store(false);
      return false;
    }
    const auto start = Clock::now();
    while (flip_pending_.load() && ElapsedMs(start) < 2000.0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return !flip_pending_.load();
  }

  int fd_ = -1;
  int wake_fd_ = -1;
  uint32_t crtc_ = 0;
  uint32_t fb_a_ = 0;
  uint32_t fb_b_ = 0;
  unsigned flip_parity_ = 0;
  drmModeConnector* conn_ = nullptr;
  drmModeModeInfo mode_{};
  std::string skip_;
  std::mutex drain_mu_;
  std::atomic<bool> flip_pending_{false};
  std::atomic<bool> reader_run_{false};
  bool waker_armed_ = false;
  std::thread reader_;
};

// Without a waker the drain's poll has nothing left to wake it: the event is
// gone and the fd never becomes readable again, so it runs the whole budget.
// This is the stall in #367, reproduced deterministically.
TEST_F(VkmsFlip, DrainBurnsItsBudgetWhenTheReaderWon) {
  if (!skip_.empty()) {
    GTEST_SKIP() << skip_;
  }
  StartReader();
  ASSERT_TRUE(FlipAndLetReaderDrain()) << "vkms delivered no flip completion";

  constexpr int kBudgetMs = 60;
  const auto start = Clock::now();
  EXPECT_FALSE(Drain(kBudgetMs, -1));
  const double waited = ElapsedMs(start);
  EXPECT_GE(waited, kBudgetMs * 0.9)
      << "expected the poll to burn its budget; took " << waited << " ms";
}

// With an eventfd in the poll set, signalled where the flag is cleared, the
// same interleaving returns at once. This is the shape of the fix.
TEST_F(VkmsFlip, AWakerInThePollSetEndsTheWaitImmediately) {
  if (!skip_.empty()) {
    GTEST_SKIP() << skip_;
  }
  waker_armed_ = true;
  StartReader();
  ASSERT_TRUE(FlipAndLetReaderDrain()) << "vkms delivered no flip completion";

  constexpr int kBudgetMs = 60;
  const auto start = Clock::now();
  EXPECT_FALSE(Drain(kBudgetMs, wake_fd_));
  const double waited = ElapsedMs(start);
  EXPECT_LT(waited, kBudgetMs / 2.0)
      << "the waker should have ended the wait at once; took " << waited
      << " ms";
}

}  // namespace
