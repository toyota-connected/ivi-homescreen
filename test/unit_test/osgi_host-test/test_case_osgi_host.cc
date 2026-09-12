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

/*
 * The FFI path end to end, minus the VM: a bundle calls the ihs_osgi_* C
 * surface in libihs_shared, the forwarders read the proc table the shell
 * installed, and the adapter drives a real BridgeRegistry.
 *
 * osgi_bridge-test covers the registry alone and ihs_osgi-test covers the
 * forwarders alone, against a mock host each. This is the only place the two
 * halves meet, so it is where a mismatch between them shows up -- a status
 * mapped wrongly, a handle not minted, a registration left behind.
 *
 * The Dart DL calls are faked for the same reason as in osgi_bridge-test: they
 * need a live VM, and nothing here depends on what they do.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ihs/ihs_osgi.h"

#include "osgi/bridge_registry.h"
#include "osgi/osgi_host.h"

namespace {

struct FakeDart {
  static inline std::vector<std::pair<int64_t, int64_t>> posts;
  static inline int init_calls = 0;

  static void Reset() {
    posts.clear();
    init_calls = 0;
  }

  static intptr_t Initialize(void*) {
    ++init_calls;
    return 0;
  }

  static bool PostSendPort(const int64_t target_port,
                           const int64_t send_port_id) {
    posts.emplace_back(target_port, send_port_id);
    return true;
  }
};

ihs::osgi::DartPortApi FakeApi() {
  return {&FakeDart::Initialize, &FakeDart::PostSendPort};
}

class RecordingObserver final : public ihs::osgi::IBundleLifecycleObserver {
 public:
  void OnBundleActive(const std::string& name) override {
    active.push_back(name);
  }
  void OnBundleStopped(const std::string& name) override {
    stopped.push_back(name);
  }
  std::vector<std::string> active;
  std::vector<std::string> stopped;
};

// Each test installs its own registry rather than the process singleton, which
// is what InstallOsgiHost taking a reference is for. The host itself is
// process-global, so it has to come back out again in teardown.
class OsgiHostTest : public ::testing::Test {
 protected:
  void SetUp() override {
    FakeDart::Reset();
    api_ = FakeApi();
    registry_ = std::make_unique<ihs::osgi::BridgeRegistry>(api_);
    registry_->SetLifecycleObserver(&observer_);
    ihs::osgi::InstallOsgiHost(*registry_);
  }

  void TearDown() override { ihs::osgi::UninstallOsgiHost(); }

  IhsOsgiPeerInfo Peer(const int64_t port) {
    IhsOsgiPeerInfo peer{};
    peer.struct_size = sizeof(IhsOsgiPeerInfo);
    peer.dart_api_dl_data = &dl_data_;
    peer.port = port;
    return peer;
  }

  IhsOsgiBundleInfo BundleInfo(const char* name, const int64_t port) {
    IhsOsgiBundleInfo info{};
    info.struct_size = sizeof(IhsOsgiBundleInfo);
    info.peer = Peer(port);
    info.symbolic_name = name;
    return info;
  }

  int dl_data_ = 0;
  ihs::osgi::DartPortApi api_{};
  RecordingObserver observer_;
  std::unique_ptr<ihs::osgi::BridgeRegistry> registry_;
};

constexpr int64_t kFrameworkPort = 1001;

}  // namespace

// Uninstalling has to be what the forwarders see, or teardown would leave the
// next test talking to a destroyed registry.
TEST_F(OsgiHostTest, UninstallingTheHostMakesTheSurfaceUnavailable) {
  EXPECT_TRUE(ihs_osgi_available());
  ihs::osgi::UninstallOsgiHost();

  EXPECT_FALSE(ihs_osgi_available());
  const IhsOsgiPeerInfo peer = Peer(kFrameworkPort);
  EXPECT_EQ(ihs_osgi_register_framework(&peer, nullptr),
            IHS_OSGI_ERR_UNAVAILABLE);
}

// The startup race, driven through the C surface: a bundle registers before the
// framework exists and is served the moment it does.
TEST_F(OsgiHostTest, RegistersTheFrameworkAndServesWaitingBundles) {
  IhsOsgiBundle* bundle = nullptr;
  const IhsOsgiBundleInfo info = BundleInfo("com.ivi.cluster", 7);
  ASSERT_EQ(ihs_osgi_register_bundle(&info, &bundle), IHS_OSGI_OK);
  EXPECT_TRUE(FakeDart::posts.empty()) << "no framework port to deliver yet";

  const IhsOsgiPeerInfo peer = Peer(kFrameworkPort);
  size_t served = 0;
  ASSERT_EQ(ihs_osgi_register_framework(&peer, &served), IHS_OSGI_OK);
  EXPECT_EQ(served, 1u);

  ASSERT_EQ(FakeDart::posts.size(), 1u);
  EXPECT_EQ(FakeDart::posts[0].first, 7);
  EXPECT_EQ(FakeDart::posts[0].second, kFrameworkPort);
}

TEST_F(OsgiHostTest, RegistersABundleAndReportsThroughItsHandle) {
  IhsOsgiBundle* bundle = nullptr;
  const IhsOsgiBundleInfo info = BundleInfo("com.ivi.cluster", 7);
  ASSERT_EQ(ihs_osgi_register_bundle(&info, &bundle), IHS_OSGI_OK);
  ASSERT_NE(bundle, nullptr);

  EXPECT_EQ(ihs_osgi_report_active(bundle), IHS_OSGI_OK);
  EXPECT_EQ(observer_.active, std::vector<std::string>{"com.ivi.cluster"});

  EXPECT_EQ(ihs_osgi_report_stopped(bundle), IHS_OSGI_OK);
  EXPECT_EQ(observer_.stopped, std::vector<std::string>{"com.ivi.cluster"});
}

// The declared set gates this path exactly as it gates the channel: both end at
// the same registry.
TEST_F(OsgiHostTest, AnUndeclaredNameIsRefused) {
  registry_->SetDeclaredBundles({"com.ivi.cluster"});

  IhsOsgiBundle* bundle = nullptr;
  const IhsOsgiBundleInfo info = BundleInfo("com.ivi.clustre", 7);
  EXPECT_EQ(ihs_osgi_register_bundle(&info, &bundle), IHS_OSGI_ERR_REJECTED);
  EXPECT_EQ(bundle, nullptr);
}

// The reason the handle exists. Anything in the process can reach these
// symbols, so a report that did not come with a minted capability must be
// refused rather than advance someone else's startup.
TEST_F(OsgiHostTest, AForgedHandleCannotReportActive) {
  IhsOsgiBundle* real = nullptr;
  const IhsOsgiBundleInfo info = BundleInfo("com.ivi.cluster", 7);
  ASSERT_EQ(ihs_osgi_register_bundle(&info, &real), IHS_OSGI_OK);

  const uintptr_t token = reinterpret_cast<uintptr_t>(real);
  auto* forged = reinterpret_cast<IhsOsgiBundle*>(token ^ 1u);
  EXPECT_EQ(ihs_osgi_report_active(forged), IHS_OSGI_ERR_REJECTED);
  EXPECT_TRUE(observer_.active.empty());
}

TEST_F(OsgiHostTest, UnregisterFreesTheNameAndInvalidatesTheHandle) {
  IhsOsgiBundle* bundle = nullptr;
  const IhsOsgiBundleInfo info = BundleInfo("com.ivi.cluster", 7);
  ASSERT_EQ(ihs_osgi_register_bundle(&info, &bundle), IHS_OSGI_OK);

  EXPECT_EQ(ihs_osgi_unregister(bundle), IHS_OSGI_OK);
  EXPECT_EQ(ihs_osgi_report_active(bundle), IHS_OSGI_ERR_REJECTED)
      << "the capability died with the registration";

  // Which is what a restart needs.
  IhsOsgiBundle* restarted = nullptr;
  const IhsOsgiBundleInfo again = BundleInfo("com.ivi.cluster", 8);
  EXPECT_EQ(ihs_osgi_register_bundle(&again, &restarted), IHS_OSGI_OK);
  EXPECT_NE(restarted, nullptr);
  EXPECT_NE(restarted, bundle);
}

// Teardown is usually running because something else already failed; a second
// failure there would bury the first.
TEST_F(OsgiHostTest, UnregisteringAnUnknownHandleIsTolerated) {
  auto* unknown = reinterpret_cast<IhsOsgiBundle*>(uintptr_t{0xDEAD});
  EXPECT_EQ(ihs_osgi_unregister(unknown), IHS_OSGI_OK);
  EXPECT_EQ(ihs_osgi_unregister(nullptr), IHS_OSGI_OK);
}
