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

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "osgi/bridge_registry.h"

namespace {

// Records what the registry would have posted to Dart. The real calls need a
// live VM; the ordering logic under test does not.
struct FakeDart {
  static inline std::vector<std::pair<int64_t, int64_t>> posts;
  static inline intptr_t init_result = 0;
  static inline int init_calls = 0;
  static inline bool post_succeeds = true;

  static void Reset() {
    posts.clear();
    init_result = 0;
    init_calls = 0;
    post_succeeds = true;
  }

  static intptr_t Initialize(void*) {
    ++init_calls;
    return init_result;
  }

  static bool Post(const int64_t port, const int64_t value) {
    if (!post_succeeds) {
      return false;
    }
    posts.emplace_back(port, value);
    return true;
  }
};

ihs::osgi::DartPortApi FakeApi() {
  return {&FakeDart::Initialize, &FakeDart::Post};
}

// A registry with the DL API already initialized, which is the precondition for
// everything except the initialization cases themselves.
class BridgeRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    FakeDart::Reset();
    api_ = FakeApi();
    registry_ = std::make_unique<ihs::osgi::BridgeRegistry>(api_);
    ASSERT_TRUE(registry_->InitializeDartApi(0xDEADBEEF));
  }

  ihs::osgi::DartPortApi api_{};
  std::unique_ptr<ihs::osgi::BridgeRegistry> registry_;
};

constexpr int64_t kFrameworkPort = 1001;

}  // namespace

// --- Dart DL initialization -------------------------------------------------

TEST(BridgeRegistryInit, RejectsNullDlData) {
  FakeDart::Reset();
  auto api = FakeApi();
  ihs::osgi::BridgeRegistry registry(api);
  EXPECT_FALSE(registry.InitializeDartApi(0));
  EXPECT_FALSE(registry.dart_api_ready());
  EXPECT_EQ(FakeDart::init_calls, 0) << "must not call into Dart with null";
}

// A non-zero return means the vendored headers disagree with the running VM.
// That has to surface as a hard failure, not a silently degraded bridge.
TEST(BridgeRegistryInit, RejectsVersionMismatch) {
  FakeDart::Reset();
  FakeDart::init_result = 1;
  auto api = FakeApi();
  ihs::osgi::BridgeRegistry registry(api);
  EXPECT_FALSE(registry.InitializeDartApi(0xDEADBEEF));
  EXPECT_FALSE(registry.dart_api_ready());
}

// Every isolate passes its DL data because any of them may arrive first; all
// but the first must be a no-op rather than a repeated bind.
TEST(BridgeRegistryInit, IsIdempotent) {
  FakeDart::Reset();
  auto api = FakeApi();
  ihs::osgi::BridgeRegistry registry(api);
  EXPECT_TRUE(registry.InitializeDartApi(0xDEADBEEF));
  EXPECT_TRUE(registry.InitializeDartApi(0xDEADBEEF));
  EXPECT_TRUE(registry.InitializeDartApi(0xCAFEF00D));
  EXPECT_EQ(FakeDart::init_calls, 1);
}

// Nothing works before the DL API is bound; failing loudly beats posting into
// an unbound symbol table.
TEST(BridgeRegistryInit, RejectsUseBeforeInitialize) {
  FakeDart::Reset();
  auto api = FakeApi();
  ihs::osgi::BridgeRegistry registry(api);
  EXPECT_FALSE(registry.RegisterBundle("com.ivi.cluster", 7));
  EXPECT_FALSE(registry.SetFrameworkPort(kFrameworkPort).has_value());
}

// --- The startup race -------------------------------------------------------
//
// A bundle engine and the framework isolate start concurrently, so `init` can
// arrive in either order. Both orders must end with the bundle holding the
// framework port, and the two tests below are the same scenario run each way.

TEST_F(BridgeRegistryTest, FrameworkFirstThenBundle) {
  ASSERT_EQ(registry_->SetFrameworkPort(kFrameworkPort), 0u)
      << "no bundles registered yet, so none served";
  ASSERT_TRUE(registry_->RegisterBundle("com.ivi.cluster", 7));

  ASSERT_EQ(FakeDart::posts.size(), 1u);
  EXPECT_EQ(FakeDart::posts[0].first, 7) << "posted to the bundle's port";
  EXPECT_EQ(FakeDart::posts[0].second, kFrameworkPort);
  EXPECT_TRUE(registry_->PendingBundles().empty());
}

TEST_F(BridgeRegistryTest, BundleFirstThenFramework) {
  ASSERT_TRUE(registry_->RegisterBundle("com.ivi.cluster", 7));
  // Nothing to deliver yet -- and this is not an error.
  EXPECT_TRUE(FakeDart::posts.empty());
  EXPECT_EQ(registry_->PendingBundles(),
            std::vector<std::string>{"com.ivi.cluster"});

  EXPECT_EQ(registry_->SetFrameworkPort(kFrameworkPort), 1u);

  ASSERT_EQ(FakeDart::posts.size(), 1u);
  EXPECT_EQ(FakeDart::posts[0].first, 7);
  EXPECT_EQ(FakeDart::posts[0].second, kFrameworkPort);
  EXPECT_TRUE(registry_->PendingBundles().empty());
}

// The realistic startup: several bundles register while the framework is still
// coming up, then all are flushed at once.
TEST_F(BridgeRegistryTest, FlushesEveryPendingBundleOnce) {
  ASSERT_TRUE(registry_->RegisterBundle("com.ivi.cluster", 7));
  ASSERT_TRUE(registry_->RegisterBundle("com.ivi.navigation", 8));
  ASSERT_TRUE(registry_->RegisterBundle("com.ivi.media", 9));
  EXPECT_EQ(registry_->PendingBundles().size(), 3u);

  EXPECT_EQ(registry_->SetFrameworkPort(kFrameworkPort), 3u);
  EXPECT_EQ(FakeDart::posts.size(), 3u);

  // Re-announcing the same framework port must not re-deliver: a bundle that
  // received the port once would otherwise see a duplicate message.
  EXPECT_EQ(registry_->SetFrameworkPort(kFrameworkPort), 0u);
  EXPECT_EQ(FakeDart::posts.size(), 3u);
}

// A bundle that registers after the flush still gets served.
TEST_F(BridgeRegistryTest, LateBundleIsServedImmediately) {
  ASSERT_TRUE(registry_->RegisterBundle("com.ivi.cluster", 7));
  ASSERT_EQ(registry_->SetFrameworkPort(kFrameworkPort), 1u);

  ASSERT_TRUE(registry_->RegisterBundle("com.ivi.late", 42));
  ASSERT_EQ(FakeDart::posts.size(), 2u);
  EXPECT_EQ(FakeDart::posts[1].first, 42);
  EXPECT_TRUE(registry_->PendingBundles().empty());
}

// --- Registration hygiene ---------------------------------------------------

TEST_F(BridgeRegistryTest, RejectsDuplicateSymbolicName) {
  ASSERT_TRUE(registry_->RegisterBundle("com.ivi.cluster", 7));
  // Silently rebinding would strand whichever isolate is no longer referenced.
  EXPECT_FALSE(registry_->RegisterBundle("com.ivi.cluster", 8));
  EXPECT_EQ(registry_->PortFor("com.ivi.cluster"), 7);
}

TEST_F(BridgeRegistryTest, RejectsInvalidPortAndEmptyName) {
  EXPECT_FALSE(registry_->RegisterBundle("com.ivi.cluster", 0));
  EXPECT_FALSE(registry_->RegisterBundle("", 7));
}

// A restart re-registers the same symbolic name, which requires an explicit
// unregister first.
TEST_F(BridgeRegistryTest, UnregisterAllowsReRegistration) {
  ASSERT_TRUE(registry_->RegisterBundle("com.ivi.cluster", 7));
  EXPECT_TRUE(registry_->UnregisterBundle("com.ivi.cluster"));
  EXPECT_FALSE(registry_->UnregisterBundle("com.ivi.cluster"));
  EXPECT_FALSE(registry_->PortFor("com.ivi.cluster").has_value());

  EXPECT_TRUE(registry_->RegisterBundle("com.ivi.cluster", 8));
  EXPECT_EQ(registry_->PortFor("com.ivi.cluster"), 8);
}

TEST_F(BridgeRegistryTest, RejectsInvalidFrameworkPort) {
  EXPECT_FALSE(registry_->SetFrameworkPort(0).has_value());
}

// Two framework isolates in one process is a startup bug. Replacing the port
// would silently strand every bundle already pointed at the first one.
TEST_F(BridgeRegistryTest, RefusesToReplaceFrameworkPort) {
  ASSERT_TRUE(registry_->SetFrameworkPort(kFrameworkPort).has_value());
  EXPECT_FALSE(registry_->SetFrameworkPort(2002).has_value());
  EXPECT_EQ(registry_->framework_port(), kFrameworkPort);
}

// --- Delivery failure -------------------------------------------------------

// A closed port means that isolate died between registering and the flush. The
// bundle stays pending rather than being marked served, so a later framework
// announcement can retry it.
TEST_F(BridgeRegistryTest, FailedPostLeavesBundlePending) {
  ASSERT_TRUE(registry_->RegisterBundle("com.ivi.cluster", 7));
  FakeDart::post_succeeds = false;

  EXPECT_EQ(registry_->SetFrameworkPort(kFrameworkPort), 0u);
  EXPECT_EQ(registry_->PendingBundles(),
            std::vector<std::string>{"com.ivi.cluster"});

  FakeDart::post_succeeds = true;
  EXPECT_EQ(registry_->SetFrameworkPort(kFrameworkPort), 1u);
  EXPECT_TRUE(registry_->PendingBundles().empty());
}

// --- Lifecycle observer ---------------------------------------------------
//
// The seam between the bridge (which knows channels) and the orchestrator
// (which knows startup sequencing). Without it a bundle can register, run its
// activator and report ACTIVE while the critical wait times out anyway.

namespace {
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
}  // namespace

TEST_F(BridgeRegistryTest, ForwardsActiveAndStoppedToTheObserver) {
  RecordingObserver observer;
  registry_->SetLifecycleObserver(&observer);
  ASSERT_TRUE(registry_->RegisterBundle("com.ivi.cluster", 7));

  EXPECT_TRUE(registry_->ReportActive("com.ivi.cluster"));
  EXPECT_EQ(observer.active, std::vector<std::string>{"com.ivi.cluster"});

  EXPECT_TRUE(registry_->ReportStopped("com.ivi.cluster"));
  EXPECT_EQ(observer.stopped, std::vector<std::string>{"com.ivi.cluster"});
}

// A report from a bundle that never called init cannot advance anything, and
// silently accepting it would hide a symbolic_name that does not match config.
TEST_F(BridgeRegistryTest, RejectsActiveFromAnUnregisteredBundle) {
  RecordingObserver observer;
  registry_->SetLifecycleObserver(&observer);
  EXPECT_FALSE(registry_->ReportActive("com.ivi.ghost"));
  EXPECT_TRUE(observer.active.empty());
}

// With no observer attached the registry must not crash -- the shell may run
// bundles with no orchestrator (every bundle deferred, nothing awaited).
TEST_F(BridgeRegistryTest, ActiveWithoutAnObserverIsHarmless) {
  ASSERT_TRUE(registry_->RegisterBundle("com.ivi.cluster", 7));
  EXPECT_TRUE(registry_->ReportActive("com.ivi.cluster"));
}

TEST_F(BridgeRegistryTest, DetachingTheObserverStopsDelivery) {
  RecordingObserver observer;
  registry_->SetLifecycleObserver(&observer);
  ASSERT_TRUE(registry_->RegisterBundle("com.ivi.cluster", 7));
  registry_->SetLifecycleObserver(nullptr);
  EXPECT_TRUE(registry_->ReportActive("com.ivi.cluster"));
  EXPECT_TRUE(observer.active.empty());
}

// The observer is dispatched outside the registry lock, so an observer that
// calls back into the registry must not deadlock. The orchestrator does
// exactly this shape of thing under its own lock.
TEST_F(BridgeRegistryTest, ObserverMayCallBackIntoTheRegistry) {
  class Reentrant final : public ihs::osgi::IBundleLifecycleObserver {
   public:
    explicit Reentrant(ihs::osgi::BridgeRegistry* r) : registry_(r) {}
    void OnBundleActive(const std::string&) override {
      (void)registry_->PendingBundles();
      (void)registry_->framework_port();
    }
    void OnBundleStopped(const std::string&) override {}

   private:
    ihs::osgi::BridgeRegistry* registry_;
  };
  Reentrant observer(registry_.get());
  registry_->SetLifecycleObserver(&observer);
  ASSERT_TRUE(registry_->RegisterBundle("com.ivi.cluster", 7));
  EXPECT_TRUE(registry_->ReportActive("com.ivi.cluster"));
}

// --- Concurrency ------------------------------------------------------------

// Bundle engines register from their own threads while the framework may land
// at any moment. Every bundle must be served exactly once regardless of
// interleaving.
TEST_F(BridgeRegistryTest, ConcurrentRegistrationServesEachBundleOnce) {
  constexpr int kBundles = 32;
  std::vector<std::thread> threads;
  threads.reserve(kBundles + 1);

  for (int i = 0; i < kBundles; ++i) {
    threads.emplace_back([this, i] {
      registry_->RegisterBundle("com.ivi.bundle" + std::to_string(i), 100 + i);
    });
  }
  threads.emplace_back([this] { registry_->SetFrameworkPort(kFrameworkPort); });

  for (auto& t : threads) {
    t.join();
  }

  // Whoever lost the race is flushed here; after this nothing may be pending.
  registry_->SetFrameworkPort(kFrameworkPort);
  EXPECT_TRUE(registry_->PendingBundles().empty());

  ASSERT_EQ(FakeDart::posts.size(), static_cast<size_t>(kBundles))
      << "each bundle served exactly once";
  std::vector<int64_t> ports;
  ports.reserve(FakeDart::posts.size());
  for (const auto& [port, value] : FakeDart::posts) {
    EXPECT_EQ(value, kFrameworkPort);
    ports.push_back(port);
  }
  std::sort(ports.begin(), ports.end());
  EXPECT_EQ(std::unique(ports.begin(), ports.end()) - ports.begin(), kBundles)
      << "no bundle served twice";
}
