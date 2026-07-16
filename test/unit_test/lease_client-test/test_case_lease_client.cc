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

// LeaseClient against a mock drm-lease-v1 compositor.
//
// A hand-rolled libwayland *server* (correct by construction: it uses the raw
// server API) implements wp_drm_lease_device_v1 / _connector_v1 / _request_v1 /
// _lease_v1; the client under test is the real homescreen::LeaseClient. It runs
// on a private socket in a temp XDG_RUNTIME_DIR, so no compositor, GPU, or
// leasable connector is needed — which is the point: on real wlroots hosts
// nothing is offered unless a connector's EDID carries the non-desktop flag, so
// the grant / monitor / teardown paths are unreachable outside this tier.
//
// The interface tables come from the generated *client* header, which emits
// them (EMIT_INTERFACE_TABLES); a wl_interface is direction-agnostic.

#include "backend/wayland_leased_drm/lease_client.h"

#include "wayland-protocols/drm_lease_v1_client.hpp"

extern "C" {
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-core.h>
}

#include <cerrno>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace proto = drm_lease_v1::client;
using homescreen::LeaseClient;
using homescreen::LeaseConfig;
using homescreen::LeaseError;

// Event opcodes, taken from the generated traits so a protocol change can't
// silently desync the mock from the client.
constexpr uint32_t kDevDrmFd = proto::wp_drm_lease_device_v1_traits::Evt::DrmFd;
constexpr uint32_t kDevConnector =
    proto::wp_drm_lease_device_v1_traits::Evt::Connector;
constexpr uint32_t kDevDone = proto::wp_drm_lease_device_v1_traits::Evt::Done;
constexpr uint32_t kDevReleased =
    proto::wp_drm_lease_device_v1_traits::Evt::Released;

constexpr uint32_t kConnName =
    proto::wp_drm_lease_connector_v1_traits::Evt::Name;
constexpr uint32_t kConnDescription =
    proto::wp_drm_lease_connector_v1_traits::Evt::Description;
constexpr uint32_t kConnConnectorId =
    proto::wp_drm_lease_connector_v1_traits::Evt::ConnectorId;
constexpr uint32_t kConnDone =
    proto::wp_drm_lease_connector_v1_traits::Evt::Done;

constexpr uint32_t kLeaseFd = proto::wp_drm_lease_v1_traits::Evt::LeaseFd;
constexpr uint32_t kLeaseFinished =
    proto::wp_drm_lease_v1_traits::Evt::Finished;

int MakeDummyFd() {
  // Stands in for the DRM fd. drmGetDeviceNameFromFd2() on it fails, so the
  // client reports the node as "(unresolved)" — which is fine here; resolving a
  // real node is the vkms tier's job, not this one's.
  const int fd = memfd_create("lease-test", MFD_CLOEXEC);
  EXPECT_GE(fd, 0);
  return fd;
}

// Post an fd event and drop our copy: the sender keeps ownership of an 'h'
// argument, so the mock must close what it created.
void PostFdAndClose(wl_resource* res, uint32_t opcode) {
  const int fd = MakeDummyFd();
  wl_resource_post_event(res, opcode, fd);
  close(fd);
}

// Number of fds this process currently holds. Used to gate fd leaks, which
// LeakSanitizer cannot see -- it tracks memory, not descriptors.
size_t CountOpenFds() {
  size_t n = 0;
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator("/proc/self/fd", ec);
       !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    ++n;
  }
  return n;
}

// What the mock should do when a lease is submitted.
enum class GrantPolicy {
  kGrant,                 // lease_fd
  kDeny,                  // finished, no lease_fd
  kGrantThenFinishNow,    // lease_fd + finished in the same flush
  kGrantThenFinishLater,  // lease_fd, then finished ~50 ms on
  kGrantTwice,            // lease_fd twice — a misbehaving/hostile compositor
};

struct ConnectorSpec {
  std::string name;
  std::string description;
  uint32_t connector_id;
};

struct DeviceSpec {
  std::vector<ConnectorSpec> connectors;
};

struct MockConfig {
  std::vector<DeviceSpec> devices{{{{"HDMI-A-1", "mock panel", 42}}}};
  GrantPolicy policy = GrantPolicy::kGrant;
  bool send_drm_fd = true;  // false = defer forever (compositor lacks master)
  bool send_device_done = true;
};

// ── mock compositor ────────────────────────────────────────────────────────

class MockCompositor {
 public:
  explicit MockCompositor(MockConfig cfg) : cfg_(std::move(cfg)) {
    // Hermetic runtime dir so the test never touches the developer's session.
    char tmpl[] = "/tmp/ivi-lease-test-XXXXXX";
    // mkdtemp returns null on failure (/tmp full or read-only in a constrained
    // CI container); constructing a std::string from it would segfault inside
    // this constructor. ADD_FAILURE rather than ASSERT_*: the latter expands to
    // a value-returning `return`, which is ill-formed in a constructor. The
    // early return leaves display_ null, which the destructor tolerates.
    const char* dir = mkdtemp(tmpl);
    if (dir == nullptr) {
      ADD_FAILURE() << "mkdtemp failed: " << std::strerror(errno);
      return;
    }
    runtime_dir_ = dir;
    prev_xdg_ = GetEnvOr("XDG_RUNTIME_DIR");
    prev_wl_ = GetEnvOr("WAYLAND_DISPLAY");
    setenv("XDG_RUNTIME_DIR", runtime_dir_.c_str(), 1);

    display_ = wl_display_create();
    EXPECT_NE(display_, nullptr);
    const char* sock = wl_display_add_socket_auto(display_);
    EXPECT_NE(sock, nullptr);
    socket_name_ = sock != nullptr ? sock : "";
    setenv("WAYLAND_DISPLAY", socket_name_.c_str(), 1);

    // One global per device spec, each carrying its own index. NOT a running
    // bind counter: every client connection binds every global, so a counter
    // would only be correct for the first client and would silently advertise
    // no connectors to the second.
    for (size_t i = 0; i < cfg_.devices.size(); ++i) {
      global_ctxs_.push_back(GlobalCtx{this, i});
      wl_global_create(display_,
                       &proto::wp_drm_lease_device_v1_traits::wl_iface(), 1,
                       &global_ctxs_.back(), &MockCompositor::BindDevice);
    }
    thread_ = std::thread([this] { wl_display_run(display_); });
  }

  // Tolerates a half-constructed mock: the constructor bails out early if
  // mkdtemp or wl_display_create fails, leaving these null/empty.
  ~MockCompositor() {
    if (display_ != nullptr) {
      wl_display_terminate(display_);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    if (display_ != nullptr) {
      wl_display_destroy(display_);
    }
    RestoreEnv("XDG_RUNTIME_DIR", prev_xdg_);
    RestoreEnv("WAYLAND_DISPLAY", prev_wl_);
    if (!runtime_dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(runtime_dir_, ec);
    }
  }

  MockCompositor(const MockCompositor&) = delete;
  MockCompositor& operator=(const MockCompositor&) = delete;

 private:
  static std::string GetEnvOr(const char* k) {
    const char* v = getenv(k);
    return v != nullptr ? std::string(v) : std::string();
  }
  static void RestoreEnv(const char* k, const std::string& v) {
    if (v.empty()) {
      unsetenv(k);
    } else {
      setenv(k, v.c_str(), 1);
    }
  }

  // Identifies which device spec a global (and each resource bound from it)
  // corresponds to.
  struct GlobalCtx {
    MockCompositor* self;
    size_t index;
  };
  using DeviceCtx = GlobalCtx;

  static void BindDevice(wl_client* client,
                         void* data,
                         uint32_t version,
                         uint32_t id) {
    auto* gctx = static_cast<GlobalCtx*>(data);
    auto* self = gctx->self;
    const size_t index = gctx->index;

    wl_resource* res = wl_resource_create(
        client, &proto::wp_drm_lease_device_v1_traits::wl_iface(),
        static_cast<int>(version), id);
    auto* ctx = new DeviceCtx{self, index};
    wl_resource_set_implementation(res, &kDeviceImpl, ctx,
                                   &MockCompositor::DestroyDeviceCtx);

    if (self->cfg_.send_drm_fd) {
      const int fd = MakeDummyFd();
      wl_resource_post_event(res, kDevDrmFd, fd);
      // The sender keeps ownership of an 'h' argument: libwayland dups it into
      // the connection's out-buffer and does NOT close the original. Verified
      // empirically -- without this close the mock leaked one memfd per bind,
      // which the fd-growth gates below then (correctly) blamed on the client.
      close(fd);
    }

    if (index < self->cfg_.devices.size()) {
      for (const auto& c : self->cfg_.devices[index].connectors) {
        wl_resource* cr = wl_resource_create(
            client, &proto::wp_drm_lease_connector_v1_traits::wl_iface(), 1, 0);
        wl_resource_set_implementation(cr, &kConnectorImpl, nullptr, nullptr);
        wl_resource_post_event(res, kDevConnector, cr);
        wl_resource_post_event(cr, kConnName, c.name.c_str());
        wl_resource_post_event(cr, kConnDescription, c.description.c_str());
        wl_resource_post_event(cr, kConnConnectorId, c.connector_id);
        wl_resource_post_event(cr, kConnDone);
      }
    }
    if (self->cfg_.send_device_done) {
      wl_resource_post_event(res, kDevDone);
    }
  }

  static void DestroyDeviceCtx(wl_resource* res) {
    delete static_cast<DeviceCtx*>(wl_resource_get_user_data(res));
  }

  // wp_drm_lease_device_v1
  static void DeviceCreateLeaseRequest(wl_client* client,
                                       wl_resource* dev,
                                       uint32_t id) {
    auto* ctx = static_cast<DeviceCtx*>(wl_resource_get_user_data(dev));
    wl_resource* req = wl_resource_create(
        client, &proto::wp_drm_lease_request_v1_traits::wl_iface(), 1, id);
    wl_resource_set_implementation(req, &kRequestImpl, ctx->self, nullptr);
  }
  static void DeviceRelease(wl_client* /*client*/, wl_resource* dev) {
    // The `released` event is the destructor — answer, then drop the resource.
    // The client drains for exactly this before destroying its proxy.
    wl_resource_post_event(dev, kDevReleased);
    wl_resource_destroy(dev);
  }
  static constexpr struct {
    void (*create_lease_request)(wl_client*, wl_resource*, uint32_t);
    void (*release)(wl_client*, wl_resource*);
  } kDeviceImpl{&MockCompositor::DeviceCreateLeaseRequest,
                &MockCompositor::DeviceRelease};

  // wp_drm_lease_connector_v1
  static void ConnectorDestroy(wl_client* /*client*/, wl_resource* r) {
    wl_resource_destroy(r);
  }
  static constexpr struct {
    void (*destroy)(wl_client*, wl_resource*);
  } kConnectorImpl{&MockCompositor::ConnectorDestroy};

  // wp_drm_lease_request_v1
  static void RequestConnector(wl_client* /*client*/,
                               wl_resource* /*req*/,
                               wl_resource* /*connector*/) {}

  // wl_display_destroy does not reap a still-armed timer source, so the source
  // owns itself: it fires once, removes itself, and frees its context.
  struct FinishCtx {
    wl_resource* lease;
    wl_event_source* src;
  };

  static int FinishLater(void* data) {
    auto* ctx = static_cast<FinishCtx*>(data);
    wl_resource_post_event(ctx->lease, kLeaseFinished);
    wl_client_flush(wl_resource_get_client(ctx->lease));
    wl_event_source_remove(ctx->src);
    delete ctx;
    return 0;
  }

  static void RequestSubmit(wl_client* client, wl_resource* req, uint32_t id) {
    auto* self = static_cast<MockCompositor*>(wl_resource_get_user_data(req));
    wl_resource* lease = wl_resource_create(
        client, &proto::wp_drm_lease_v1_traits::wl_iface(), 1, id);
    wl_resource_set_implementation(lease, &kLeaseImpl, nullptr, nullptr);
    // submit is a destructor request: the request object is gone server-side.
    wl_resource_destroy(req);

    switch (self->cfg_.policy) {
      case GrantPolicy::kDeny:
        wl_resource_post_event(lease, kLeaseFinished);
        break;
      case GrantPolicy::kGrant:
        PostFdAndClose(lease, kLeaseFd);
        break;
      case GrantPolicy::kGrantThenFinishNow:
        PostFdAndClose(lease, kLeaseFd);
        wl_resource_post_event(lease, kLeaseFinished);
        break;
      case GrantPolicy::kGrantTwice:
        // Protocol-violating, but nothing on the wire stops a compositor doing
        // it, and the client must not drop the superseded fd on the floor.
        PostFdAndClose(lease, kLeaseFd);
        PostFdAndClose(lease, kLeaseFd);
        break;
      case GrantPolicy::kGrantThenFinishLater: {
        PostFdAndClose(lease, kLeaseFd);
        auto* ctx = new FinishCtx{lease, nullptr};
        ctx->src =
            wl_event_loop_add_timer(wl_display_get_event_loop(self->display_),
                                    &MockCompositor::FinishLater, ctx);
        wl_event_source_timer_update(ctx->src, 50);
        break;
      }
    }
  }
  static constexpr struct {
    void (*request_connector)(wl_client*, wl_resource*, wl_resource*);
    void (*submit)(wl_client*, wl_resource*, uint32_t);
  } kRequestImpl{&MockCompositor::RequestConnector,
                 &MockCompositor::RequestSubmit};

  // wp_drm_lease_v1
  static void LeaseDestroy(wl_client* /*client*/, wl_resource* r) {
    wl_resource_destroy(r);
  }
  static constexpr struct {
    void (*destroy)(wl_client*, wl_resource*);
  } kLeaseImpl{&MockCompositor::LeaseDestroy};

  MockConfig cfg_;
  wl_display* display_ = nullptr;
  std::thread thread_;
  std::string runtime_dir_;
  std::string socket_name_;
  std::string prev_xdg_;
  std::string prev_wl_;
  // Stable addresses for wl_global_create's user data; deque never reallocates
  // its elements.
  std::deque<GlobalCtx> global_ctxs_;
};

LeaseConfig Cfg(uint32_t timeout_ms = 2000) {
  LeaseConfig c;
  c.timeout_ms = timeout_ms;
  return c;
}

// ── tests ──────────────────────────────────────────────────────────────────

TEST(LeaseClient, GrantsALeaseAndReportsTheConnector) {
  MockCompositor mock{MockConfig{}};

  auto res = LeaseClient::Acquire(Cfg());

  ASSERT_NE(res.hold, nullptr) << homescreen::ToString(res.error);
  EXPECT_EQ(res.error, LeaseError::kNone);
  EXPECT_EQ(res.hold->connector_name(), "HDMI-A-1");
  EXPECT_EQ(res.hold->connector_id(), 42u);
  EXPECT_GE(res.hold->fd(), 0);
  EXPECT_FALSE(res.hold->revoked());
  ASSERT_EQ(res.offers.size(), 1u);
  EXPECT_EQ(res.offers[0].description, "mock panel");
}

TEST(LeaseClient, TeardownReleasesCleanly) {
  MockCompositor mock{MockConfig{}};
  auto res = LeaseClient::Acquire(Cfg());
  ASSERT_NE(res.hold, nullptr) << homescreen::ToString(res.error);
  const int fd = res.hold->fd();
  EXPECT_GE(fd, 0);

  // Exercises ~LeaseHold: stop+join the monitor, destroy the lease, close the
  // fd, run the release -> released handshake, disconnect. Must not hang.
  res.hold.reset();

  // The lease fd is ours and must be closed by the destructor.
  EXPECT_EQ(fcntl(fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
}

// fd-leak gates. LeakSanitizer tracks memory, not descriptors, so these are the
// only thing standing between an fd leak and EMFILE on a long-running unit.
//
// Measured as GROWTH across repeated cycles rather than an absolute
// before/after delta: the mock server shares this process, and it reaps each
// client's accepted socket asynchronously on its own thread, so a single
// cycle's count is both offset and racy. A real leak is per-cycle and dominates
// that noise; a constant offset does not.
constexpr int kFdCycles = 8;
constexpr size_t kFdSlack = 2;  // server-side reaping lag

// The mock server reaps a disconnected client's socket on its own event-loop
// thread, so give it a moment to catch up before counting — otherwise its
// backlog of unreaped sockets is indistinguishable from a client-side leak.
void SettleServer() {
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

size_t FdGrowthOverCycles(const std::function<void()>& cycle) {
  cycle();  // warm up: first pass allocates the steady-state structures
  SettleServer();
  const size_t before = CountOpenFds();
  for (int i = 0; i < kFdCycles; ++i) {
    cycle();
  }
  SettleServer();
  const size_t after = CountOpenFds();
  return after > before ? after - before : 0;
}

TEST(LeaseClient, GrantAndTeardownLeakNoFds) {
  MockCompositor mock{MockConfig{}};
  const size_t growth = FdGrowthOverCycles([] {
    auto res = LeaseClient::Acquire(Cfg());
    ASSERT_NE(res.hold, nullptr) << homescreen::ToString(res.error);
  });
  EXPECT_LE(growth, kFdSlack) << "fds grew by " << growth << " across "
                              << kFdCycles << " grant/teardown cycles";
}

// A compositor that re-sends lease_fd must not leak the superseded descriptor.
// Unfixed, OnLeaseFd clobbers the field and one fd escapes per event — a
// hostile compositor could loop this to drive the embedder to EMFILE.
TEST(LeaseClient, RepeatedLeaseFdDoesNotLeakTheSupersededFd) {
  MockConfig cfg;
  cfg.policy = GrantPolicy::kGrantTwice;
  MockCompositor mock{cfg};
  const size_t growth = FdGrowthOverCycles([] {
    auto res = LeaseClient::Acquire(Cfg());
    ASSERT_NE(res.hold, nullptr) << homescreen::ToString(res.error);
    EXPECT_GE(res.hold->fd(), 0);
  });
  EXPECT_LE(growth, kFdSlack)
      << "fds grew by " << growth << " across " << kFdCycles
      << " cycles — the superseded lease_fd is leaking";
}

// A denial must not leak the enumeration fd either.
TEST(LeaseClient, DenialLeaksNoFds) {
  MockConfig cfg;
  cfg.policy = GrantPolicy::kDeny;
  MockCompositor mock{cfg};
  const size_t growth = FdGrowthOverCycles([] {
    auto res = LeaseClient::Acquire(Cfg());
    ASSERT_EQ(res.hold, nullptr);
  });
  EXPECT_LE(growth, kFdSlack) << "fds grew by " << growth << " across "
                              << kFdCycles << " denied cycles";
}

TEST(LeaseClient, DenialIsReportedAsDenied) {
  MockConfig cfg;
  cfg.policy = GrantPolicy::kDeny;
  MockCompositor mock{cfg};

  auto res = LeaseClient::Acquire(Cfg());

  EXPECT_EQ(res.hold, nullptr);
  EXPECT_EQ(res.error, LeaseError::kDenied);
}

// The reviewer's scenario: lease_fd and finished land in the same dispatch
// batch, so the monitor never sees a fresh event. revoked() must still latch.
TEST(LeaseClient, FinishedInSameBatchAsGrantStillLatchesRevoked) {
  MockConfig cfg;
  cfg.policy = GrantPolicy::kGrantThenFinishNow;
  MockCompositor mock{cfg};

  auto res = LeaseClient::Acquire(Cfg());

  ASSERT_NE(res.hold, nullptr) << homescreen::ToString(res.error);
  EXPECT_TRUE(res.hold->revoked())
      << "finished arrived with lease_fd but revoked() never latched";
}

// Revocation after the grant must be seen by the monitor thread.
TEST(LeaseClient, MonitorLatchesRevokedOnLaterFinished) {
  MockConfig cfg;
  cfg.policy = GrantPolicy::kGrantThenFinishLater;
  MockCompositor mock{cfg};

  auto res = LeaseClient::Acquire(Cfg());
  ASSERT_NE(res.hold, nullptr) << homescreen::ToString(res.error);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!res.hold->revoked() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(res.hold->revoked()) << "monitor thread never observed finished";
}

TEST(LeaseClient, NoConnectorsOfferedIsReported) {
  MockConfig cfg;
  cfg.devices = {DeviceSpec{{}}};  // device advertises zero connectors
  MockCompositor mock{cfg};

  auto res = LeaseClient::Acquire(Cfg());

  EXPECT_EQ(res.hold, nullptr);
  EXPECT_EQ(res.error, LeaseError::kNoOffers);
  EXPECT_TRUE(res.offers.empty());
}

TEST(LeaseClient, SeveralConnectorsWithNoSelectionIsAmbiguous) {
  MockConfig cfg;
  cfg.devices = {DeviceSpec{{{"HDMI-A-1", "a", 42}, {"DP-1", "b", 43}}}};
  MockCompositor mock{cfg};

  auto res = LeaseClient::Acquire(Cfg());

  EXPECT_EQ(res.hold, nullptr);
  EXPECT_EQ(res.error, LeaseError::kConnectorAmbiguous);
  EXPECT_EQ(res.offers.size(), 2u);
}

TEST(LeaseClient, SelectsTheNamedConnector) {
  MockConfig cfg;
  cfg.devices = {DeviceSpec{{{"HDMI-A-1", "a", 42}, {"DP-1", "b", 43}}}};
  MockCompositor mock{cfg};

  LeaseConfig c = Cfg();
  c.connector = "DP-1";
  auto res = LeaseClient::Acquire(c);

  ASSERT_NE(res.hold, nullptr) << homescreen::ToString(res.error);
  EXPECT_EQ(res.hold->connector_name(), "DP-1");
  EXPECT_EQ(res.hold->connector_id(), 43u);
}

TEST(LeaseClient, UnknownConnectorNameIsReported) {
  MockCompositor mock{MockConfig{}};

  LeaseConfig c = Cfg();
  c.connector = "VGA-9";
  auto res = LeaseClient::Acquire(c);

  EXPECT_EQ(res.hold, nullptr);
  EXPECT_EQ(res.error, LeaseError::kConnectorNotFound);
}

TEST(LeaseClient, SeveralDevicesWithNoSelectionIsAmbiguous) {
  MockConfig cfg;
  cfg.devices = {DeviceSpec{{{"HDMI-A-1", "a", 42}}},
                 DeviceSpec{{{"DP-1", "b", 43}}}};
  MockCompositor mock{cfg};

  auto res = LeaseClient::Acquire(Cfg());

  EXPECT_EQ(res.hold, nullptr);
  EXPECT_EQ(res.error, LeaseError::kDeviceAmbiguous);
}

TEST(LeaseClient, DeviceSelectableByIndex) {
  MockConfig cfg;
  cfg.devices = {DeviceSpec{{{"HDMI-A-1", "a", 42}}},
                 DeviceSpec{{{"DP-1", "b", 43}}}};
  MockCompositor mock{cfg};

  LeaseConfig c = Cfg();
  c.device = "1";
  auto res = LeaseClient::Acquire(c);

  ASSERT_NE(res.hold, nullptr) << homescreen::ToString(res.error);
  EXPECT_EQ(res.hold->connector_name(), "DP-1");
}

TEST(LeaseClient, UnknownDeviceIndexIsReported) {
  MockCompositor mock{MockConfig{}};

  LeaseConfig c = Cfg();
  c.device = "7";
  auto res = LeaseClient::Acquire(c);

  EXPECT_EQ(res.hold, nullptr);
  EXPECT_EQ(res.error, LeaseError::kDeviceNotFound);
}

// The spec lets a compositor defer drm_fd until it regains DRM master. Without
// a bound deadline this would hang startup forever, so the timeout is
// load-bearing rather than cosmetic.
TEST(LeaseClient, DeferredEnumerationHitsTheDeadlineInsteadOfHanging) {
  MockConfig cfg;
  cfg.send_device_done = false;  // never completes enumeration
  MockCompositor mock{cfg};

  const auto start = std::chrono::steady_clock::now();
  auto res = LeaseClient::Acquire(Cfg(300));
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_EQ(res.hold, nullptr);
  EXPECT_EQ(res.error, LeaseError::kTimeout);
  EXPECT_GE(elapsed, std::chrono::milliseconds(250));
  EXPECT_LT(elapsed, std::chrono::seconds(5)) << "deadline did not bind";
}

TEST(LeaseClient, ProbeEnumeratesWithoutLeasing) {
  MockCompositor mock{MockConfig{}};

  auto res = LeaseClient::Probe(Cfg());

  EXPECT_EQ(res.hold, nullptr);  // Probe never negotiates
  EXPECT_EQ(res.error, LeaseError::kNone) << homescreen::ToString(res.error);
  ASSERT_EQ(res.offers.size(), 1u);
  EXPECT_EQ(res.offers[0].name, "HDMI-A-1");
  EXPECT_EQ(res.offers[0].connector_id, 42u);
}

TEST(LeaseClient, ProbeWithNoOffersIsDistinctFromSuccess) {
  MockConfig cfg;
  cfg.devices = {DeviceSpec{{}}};
  MockCompositor mock{cfg};

  auto res = LeaseClient::Probe(Cfg());

  EXPECT_EQ(res.error, LeaseError::kNoOffers);
  EXPECT_TRUE(res.offers.empty());
}

// No mock at all: a stale WAYLAND_DISPLAY must not be trusted.
TEST(LeaseClient, StaleWaylandDisplayIsNotASession) {
  const char* prev = getenv("WAYLAND_DISPLAY");
  const std::string saved = prev != nullptr ? prev : "";
  setenv("WAYLAND_DISPLAY", "ivi-lease-test-nonexistent", 1);

  auto res = LeaseClient::Acquire(Cfg(200));

  EXPECT_EQ(res.hold, nullptr);
  EXPECT_EQ(res.error, LeaseError::kNoWaylandSession);

  if (saved.empty()) {
    unsetenv("WAYLAND_DISPLAY");
  } else {
    setenv("WAYLAND_DISPLAY", saved.c_str(), 1);
  }
}

}  // namespace
