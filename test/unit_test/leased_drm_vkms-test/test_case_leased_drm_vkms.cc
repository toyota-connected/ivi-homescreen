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

// Kernel-real DRM lease, no compositor.
//
// The mock tier proves the drm-lease-v1 *protocol* but hands out a memfd:
// nothing there is a DRM object, so every consumer of the lease fd (the adopted
// DrmDisplay, the leased DrmDumbSink) goes untested. This tier closes that gap
// from the other side: it makes a genuine lease with drmModeCreateLease on a
// vkms master fd and feeds it to the real code. No compositor is involved,
// which also sidesteps the reason none of this is reachable on a desktop --
// wlroots only offers a connector for leasing when its EDID carries the
// non-desktop flag, and vkms's Virtual-1 does not.
//
// Requirements, both checked at runtime with GTEST_SKIP rather than a failure:
//   - vkms loaded            (sudo modprobe vkms)
//   - DRM master on its node (root: DRM grants master to the first opener, and
//                             on a logind seat that is logind, so an
//                             unprivileged process gets a non-master fd and
//                             drmSetMaster returns EACCES)
//
//   sudo ./homescreen_leased_drm_vkms_ut_test_driver

#include "backend/software/drm_dumb_sink.h"
#include "display/drm_output_provider.h"

extern "C" {
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
}

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

// A vkms card, mastered, with a lease carved out of it. Everything is skipped
// rather than failed when the environment can't supply that.
class VkmsLease {
 public:
  bool Setup() {
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
      card_path_ = path;
      card_fd_ = fd;
      break;
    }
    if (card_fd_ < 0) {
      skip_reason_ = "no vkms node found (sudo modprobe vkms)";
      return false;
    }

    // DRM hands master to the first opener; on a logind seat that is logind, so
    // an unprivileged process lands here with a non-master fd.
    if (drmIsMaster(card_fd_) == 0 && drmSetMaster(card_fd_) != 0) {
      skip_reason_ = std::string("not DRM master on ") + card_path_ + " (" +
                     std::strerror(errno) +
                     ") — run this binary as root, e.g. `sudo " +
                     "./homescreen_leased_drm_vkms_ut_test_driver`";
      return false;
    }

    drmModeRes* res = drmModeGetResources(card_fd_);
    if (res == nullptr) {
      skip_reason_ = "drmModeGetResources failed on vkms";
      return false;
    }
    card_connectors_ = res->count_connectors;
    for (int i = 0; i < res->count_connectors; ++i) {
      drmModeConnector* c = drmModeGetConnector(card_fd_, res->connectors[i]);
      if (c != nullptr && c->connection == DRM_MODE_CONNECTED &&
          c->count_modes > 0) {
        connector_id_ = c->connector_id;
        drmModeFreeConnector(c);
        break;
      }
      if (c != nullptr) {
        drmModeFreeConnector(c);
      }
    }
    if (res->count_crtcs > 0) {
      crtc_id_ = res->crtcs[0];
    }
    drmModeFreeResources(res);
    if (connector_id_ == 0 || crtc_id_ == 0) {
      skip_reason_ = "vkms exposed no connected connector + crtc";
      return false;
    }

    uint32_t objects[2] = {connector_id_, crtc_id_};
    uint32_t lessee = 0;
    lease_fd_ = drmModeCreateLease(card_fd_, objects, 2, O_CLOEXEC, &lessee);
    if (lease_fd_ < 0) {
      skip_reason_ =
          std::string("drmModeCreateLease: ") + std::strerror(-lease_fd_);
      return false;
    }
    lessee_id_ = lessee;
    return true;
  }

  ~VkmsLease() {
    if (lease_fd_ >= 0) {
      ::close(lease_fd_);
    }
    if (card_fd_ >= 0) {
      ::close(card_fd_);
    }
  }

  [[nodiscard]] const std::string& skip_reason() const { return skip_reason_; }
  [[nodiscard]] int card_fd() const { return card_fd_; }
  [[nodiscard]] int lease_fd() const { return lease_fd_; }
  [[nodiscard]] uint32_t connector_id() const { return connector_id_; }
  [[nodiscard]] uint32_t lessee_id() const { return lessee_id_; }
  [[nodiscard]] const std::string& card_path() const { return card_path_; }
  [[nodiscard]] int card_connectors() const { return card_connectors_; }

 private:
  std::string skip_reason_{};
  std::string card_path_{};
  int card_fd_ = -1;
  int lease_fd_ = -1;
  uint32_t connector_id_ = 0;
  uint32_t crtc_id_ = 0;
  uint32_t lessee_id_ = 0;
  int card_connectors_ = 0;
};

// Print the reason as well as skipping: gtest does not surface a GTEST_SKIP
// message on the console, and a silent skip tells an operator nothing about
// what their environment is missing.
#define REQUIRE_VKMS_LEASE(v)                                       \
  VkmsLease v;                                                      \
  if (!(v).Setup()) {                                               \
    std::cout << "[   SKIP   ] " << (v).skip_reason() << std::endl; \
    GTEST_SKIP() << (v).skip_reason();                              \
  }

// The invariant the whole design rests on: the kernel filters a lease
// fd's resource view to the leased objects. Every "by construction" claim in
// the leased display and sink depends on this being true, and nothing has ever
// checked it against a real kernel.
TEST(LeasedDrmVkms, LeaseFdResourceViewIsKernelFiltered) {
  REQUIRE_VKMS_LEASE(v);

  drmModeRes* lres = drmModeGetResources(v.lease_fd());
  ASSERT_NE(lres, nullptr) << std::strerror(errno);
  EXPECT_EQ(lres->count_connectors, 1)
      << "lease fd should see exactly the leased connector";
  if (lres->count_connectors == 1) {
    EXPECT_EQ(lres->connectors[0], v.connector_id());
  }
  drmModeFreeResources(lres);

  EXPECT_NE(drmIsMaster(v.lease_fd()), 0)
      << "a lease fd is master over its object set by construction";
}

// DrmOutputProvider must enumerate through the supplied fd, not by re-opening
// the card: that is both the correctly-scoped view and the only handle a leased
// client is guaranteed to hold.
TEST(LeasedDrmVkms, OutputProviderEnumeratesThroughTheLeaseFd) {
  REQUIRE_VKMS_LEASE(v);

  homescreen::DrmOutputProvider provider(v.card_path());
  provider.SetEnumerationFd(v.lease_fd());
  const auto outputs = provider.EnumerateOutputs();

  ASSERT_EQ(outputs.size(), 1u)
      << "expected exactly the leased connector, got " << outputs.size();
  EXPECT_EQ(outputs[0].handle, v.connector_id());
  EXPECT_TRUE(outputs[0].connected);
  EXPECT_GT(outputs[0].width_px, 0);
  EXPECT_GT(outputs[0].height_px, 0);
}

// Without the fd it falls back to opening the card, which sees everything --
// this is the failure mode the fd path exists to prevent, pinned so a
// regression is loud.
TEST(LeasedDrmVkms, OutputProviderWithoutFdSeesTheWholeCard) {
  REQUIRE_VKMS_LEASE(v);

  homescreen::DrmOutputProvider provider(v.card_path());
  const auto outputs = provider.EnumerateOutputs();
  EXPECT_EQ(static_cast<int>(outputs.size()), v.card_connectors())
      << "open-by-path should see the whole card, not the lease";
}

// The leased software tier, end to end on a real lease: probe, mode-pick,
// dumb-buffer alloc and modeset all against KMS objects we hold by lease.
TEST(LeasedDrmVkms, DumbSinkDrivesALeasedConnector) {
  REQUIRE_VKMS_LEASE(v);

  auto sink = DrmDumbSink::Create(v.lease_fd(), /*fd_owner=*/nullptr,
                                  v.connector_id(), /*revoked=*/nullptr);
  ASSERT_NE(sink, nullptr) << "the sink could not drive the leased connector";

  const auto native = sink->NativeSize();
  ASSERT_TRUE(native.has_value());
  EXPECT_GT(native->first, 0u);
  EXPECT_GT(native->second, 0u);
  EXPECT_GT(sink->refresh_rate_hz(), 0.0);
}

// Pinning a connector that is not in the lease must fail rather than silently
// falling back to another panel.
TEST(LeasedDrmVkms, DumbSinkRefusesAConnectorOutsideTheLease) {
  REQUIRE_VKMS_LEASE(v);

  auto sink = DrmDumbSink::Create(v.lease_fd(), /*fd_owner=*/nullptr,
                                  /*connector_id=*/0xBADC0DE,
                                  /*revoked=*/nullptr);
  EXPECT_EQ(sink, nullptr)
      << "a connector outside the lease must not silently resolve to another";
}

// The sink must not close a borrowed lease fd: the LeaseHold owns it.
TEST(LeasedDrmVkms, DumbSinkDoesNotCloseTheBorrowedLeaseFd) {
  REQUIRE_VKMS_LEASE(v);

  {
    auto sink =
        DrmDumbSink::Create(v.lease_fd(), nullptr, v.connector_id(), nullptr);
    ASSERT_NE(sink, nullptr);
  }
  // Still ours, still usable — a double close would have made this EBADF.
  EXPECT_NE(fcntl(v.lease_fd(), F_GETFD), -1)
      << "the sink closed a borrowed fd: " << std::strerror(errno);
}

// Revocation: what the kernel actually does to a live lessee, and whether the
// gate added for it holds. This checks the claim -- "the fd stays open and
// valid; ioctls naming the vanished objects fail" -- checked rather than
// assumed.
TEST(LeasedDrmVkms, RevokedLeaseLosesItsObjectsButKeepsTheFd) {
  REQUIRE_VKMS_LEASE(v);

  ASSERT_EQ(drmModeRevokeLease(v.card_fd(), v.lessee_id()), 0)
      << std::strerror(errno);

  // The fd survives revocation...
  EXPECT_NE(fcntl(v.lease_fd(), F_GETFD), -1)
      << "the lease fd should stay open after revocation";

  // ...but its KMS objects are gone, which is exactly why Present() must gate.
  drmModeRes* lres = drmModeGetResources(v.lease_fd());
  if (lres != nullptr) {
    EXPECT_EQ(lres->count_connectors, 0)
        << "a revoked lease should expose no connectors";
    drmModeFreeResources(lres);
  }
}

// A sink built on a revoked lease must fail to construct rather than spray
// ioctl errors.
TEST(LeasedDrmVkms, DumbSinkRefusesARevokedLease) {
  REQUIRE_VKMS_LEASE(v);

  ASSERT_EQ(drmModeRevokeLease(v.card_fd(), v.lessee_id()), 0)
      << std::strerror(errno);

  auto sink =
      DrmDumbSink::Create(v.lease_fd(), nullptr, v.connector_id(), nullptr);
  EXPECT_EQ(sink, nullptr) << "a revoked lease has no KMS objects to drive";
}

}  // namespace
