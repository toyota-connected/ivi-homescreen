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

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "osgi/vsync_coordinator.h"

namespace {

using ihs::osgi::Priority;
using ihs::osgi::VsyncCoordinator;

// Records the ticks it receives. Shared across threads in the concurrency
// cases, so the counters are atomic and the log is mutex-guarded.
class Recorder final : public ihs::osgi::IVsyncTarget {
 public:
  explicit Recorder(std::string name, std::vector<std::string>* order = nullptr)
      : name_(std::move(name)), order_(order) {}

  void OnVsync(const uint64_t frame_start_ns) override {
    ++count_;
    last_ns_ = frame_start_ns;
    if (order_ != nullptr) {
      std::lock_guard lock(mutex_);
      order_->push_back(name_);
    }
  }

  [[nodiscard]] int count() const { return count_.load(); }
  [[nodiscard]] uint64_t last_ns() const { return last_ns_.load(); }

 private:
  std::string name_;
  std::vector<std::string>* order_;
  std::atomic<int> count_{0};
  std::atomic<uint64_t> last_ns_{0};
  static inline std::mutex mutex_;
};

std::shared_ptr<Recorder> MakeRecorder(
    const std::string& name,
    std::vector<std::string>* order = nullptr) {
  return std::make_shared<Recorder>(name, order);
}

constexpr uint64_t kFrameNs = 1'234'567'890;

}  // namespace

TEST(VsyncCoordinator, EmptyTickDeliversNothing) {
  VsyncCoordinator coordinator;
  EXPECT_EQ(coordinator.Tick(kFrameNs), 0u);
  EXPECT_EQ(coordinator.size(), 0u);
}

TEST(VsyncCoordinator, DeliversToEveryTargetWithTheSameTimestamp) {
  VsyncCoordinator coordinator;
  const auto a = MakeRecorder("a");
  const auto b = MakeRecorder("b");
  ASSERT_TRUE(coordinator.Add("a", Priority::kNormal, a));
  ASSERT_TRUE(coordinator.Add("b", Priority::kNormal, b));

  EXPECT_EQ(coordinator.Tick(kFrameNs), 2u);
  EXPECT_EQ(a->count(), 1);
  EXPECT_EQ(b->count(), 1);
  // One vblank is one instant: every engine must be told the same frame start,
  // or their frame_target_time calculations diverge against one display.
  EXPECT_EQ(a->last_ns(), kFrameNs);
  EXPECT_EQ(b->last_ns(), kFrameNs);
}

// The reason the coordinator exists: a cluster must get its baton before an
// infotainment view competes for the same vblank.
TEST(VsyncCoordinator, DispatchesCriticalBeforeNormalBeforeBackground) {
  VsyncCoordinator coordinator;
  std::vector<std::string> order;

  // Registered deliberately in the wrong order.
  ASSERT_TRUE(coordinator.Add("telemetry", Priority::kBackground,
                              MakeRecorder("telemetry", &order)));
  ASSERT_TRUE(coordinator.Add("navigation", Priority::kNormal,
                              MakeRecorder("navigation", &order)));
  ASSERT_TRUE(coordinator.Add("cluster", Priority::kCritical,
                              MakeRecorder("cluster", &order)));

  EXPECT_EQ(coordinator.DispatchOrder(),
            (std::vector<std::string>{"cluster", "navigation", "telemetry"}));

  ASSERT_EQ(coordinator.Tick(kFrameNs), 3u);
  EXPECT_EQ(order,
            (std::vector<std::string>{"cluster", "navigation", "telemetry"}));
}

// Ties break by registration order, so the dispatch sequence is stable run to
// run rather than dependent on container hashing.
TEST(VsyncCoordinator, TiesBreakByRegistrationOrder) {
  VsyncCoordinator coordinator;
  std::vector<std::string> order;
  for (const char* name : {"first", "second", "third", "fourth"}) {
    ASSERT_TRUE(
        coordinator.Add(name, Priority::kCritical, MakeRecorder(name, &order)));
  }
  ASSERT_EQ(coordinator.Tick(kFrameNs), 4u);
  EXPECT_EQ(order,
            (std::vector<std::string>{"first", "second", "third", "fourth"}));
}

// A late critical bundle still outranks normal ones already registered.
TEST(VsyncCoordinator, LateCriticalStillDispatchesFirst) {
  VsyncCoordinator coordinator;
  std::vector<std::string> order;
  ASSERT_TRUE(coordinator.Add("navigation", Priority::kNormal,
                              MakeRecorder("navigation", &order)));
  ASSERT_TRUE(coordinator.Add("media", Priority::kNormal,
                              MakeRecorder("media", &order)));
  ASSERT_TRUE(coordinator.Add("alarms", Priority::kCritical,
                              MakeRecorder("alarms", &order)));

  ASSERT_EQ(coordinator.Tick(kFrameNs), 3u);
  EXPECT_EQ(order, (std::vector<std::string>{"alarms", "navigation", "media"}));
}

TEST(VsyncCoordinator, RejectsDuplicateEmptyAndNull) {
  VsyncCoordinator coordinator;
  ASSERT_TRUE(
      coordinator.Add("cluster", Priority::kCritical, MakeRecorder("cluster")));
  // Rebinding would leave the previous engine's baton unanswered.
  EXPECT_FALSE(
      coordinator.Add("cluster", Priority::kNormal, MakeRecorder("cluster")));
  EXPECT_FALSE(coordinator.Add("", Priority::kNormal, MakeRecorder("x")));
  EXPECT_FALSE(coordinator.Add("null", Priority::kNormal, nullptr));
  EXPECT_EQ(coordinator.size(), 1u);
}

TEST(VsyncCoordinator, RemoveStopsDelivery) {
  VsyncCoordinator coordinator;
  const auto a = MakeRecorder("a");
  const auto b = MakeRecorder("b");
  ASSERT_TRUE(coordinator.Add("a", Priority::kNormal, a));
  ASSERT_TRUE(coordinator.Add("b", Priority::kNormal, b));
  ASSERT_EQ(coordinator.Tick(kFrameNs), 2u);

  EXPECT_TRUE(coordinator.Remove("a"));
  EXPECT_FALSE(coordinator.Remove("a")) << "second remove is a no-op";
  EXPECT_FALSE(coordinator.Contains("a"));

  EXPECT_EQ(coordinator.Tick(kFrameNs), 1u);
  EXPECT_EQ(a->count(), 1) << "removed target must not be ticked again";
  EXPECT_EQ(b->count(), 2);
}

// A bundle stops and restarts; the same name must be usable again, and lands at
// the back of its priority band.
TEST(VsyncCoordinator, ReAddAfterRemoveGoesToBackOfItsBand) {
  VsyncCoordinator coordinator;
  std::vector<std::string> order;
  ASSERT_TRUE(
      coordinator.Add("a", Priority::kNormal, MakeRecorder("a", &order)));
  ASSERT_TRUE(
      coordinator.Add("b", Priority::kNormal, MakeRecorder("b", &order)));
  ASSERT_TRUE(coordinator.Remove("a"));
  ASSERT_TRUE(
      coordinator.Add("a", Priority::kNormal, MakeRecorder("a", &order)));

  ASSERT_EQ(coordinator.Tick(kFrameNs), 2u);
  EXPECT_EQ(order, (std::vector<std::string>{"b", "a"}));
}

// A target that calls back into the coordinator must not deadlock: the lock is
// released before dispatch precisely so this is legal.
TEST(VsyncCoordinator, TargetMayReenterCoordinatorDuringDispatch) {
  VsyncCoordinator coordinator;

  class Reentrant final : public ihs::osgi::IVsyncTarget {
   public:
    explicit Reentrant(VsyncCoordinator* c) : coordinator_(c) {}
    void OnVsync(uint64_t) override {
      // Any of these would deadlock if Tick held the lock across dispatch.
      (void)coordinator_->size();
      (void)coordinator_->Contains("self");
      coordinator_->Remove("later");
    }

   private:
    VsyncCoordinator* coordinator_;
  };

  ASSERT_TRUE(coordinator.Add("self", Priority::kCritical,
                              std::make_shared<Reentrant>(&coordinator)));
  ASSERT_TRUE(
      coordinator.Add("later", Priority::kNormal, MakeRecorder("later")));

  EXPECT_EQ(coordinator.Tick(kFrameNs), 2u);
  // "later" was removed mid-tick but the snapshot kept it alive and delivered.
  EXPECT_FALSE(coordinator.Contains("later"));
  EXPECT_EQ(coordinator.Tick(kFrameNs), 1u);
}

// The lifetime rule: a target deregistered *and* dropped by its owner partway
// through a tick must stay alive until the dispatch that already picked it up
// finishes. Without the snapshot holding a strong reference this is a
// use-after-free on the frame path -- the exact shape of bug that only shows up
// as an occasional crash under teardown.
//
// The tick count is kept in a counter that outlives the target, since the point
// of the test is that the target itself is gone by the time we assert.
TEST(VsyncCoordinator, SnapshotKeepsRemovedTargetAliveForTheTickInFlight) {
  VsyncCoordinator coordinator;
  std::atomic<int> victim_ticks{0};

  class Victim final : public ihs::osgi::IVsyncTarget {
   public:
    explicit Victim(std::atomic<int>* ticks) : ticks_(ticks) {}
    void OnVsync(uint64_t) override { ++*ticks_; }

   private:
    std::atomic<int>* ticks_;
  };

  auto victim = std::make_shared<Victim>(&victim_ticks);
  std::weak_ptr<Victim> weak = victim;

  class Dropper final : public ihs::osgi::IVsyncTarget {
   public:
    Dropper(VsyncCoordinator* c, std::shared_ptr<Victim>* owner)
        : coordinator_(c), owner_(owner) {}
    void OnVsync(uint64_t) override {
      // Simulates the bundle being torn down from another path mid-frame:
      // deregistered, and the last external reference dropped.
      coordinator_->Remove("victim");
      owner_->reset();
    }

   private:
    VsyncCoordinator* coordinator_;
    std::shared_ptr<Victim>* owner_;
  };

  ASSERT_TRUE(
      coordinator.Add("dropper", Priority::kCritical,
                      std::make_shared<Dropper>(&coordinator, &victim)));
  ASSERT_TRUE(coordinator.Add("victim", Priority::kNormal, victim));
  victim.reset();  // the snapshot and the local `victim` are the only refs

  // Dispatches to dropper (which unregisters victim and drops the registry's
  // reference), then still safely to victim off the snapshot.
  EXPECT_EQ(coordinator.Tick(kFrameNs), 2u);
  EXPECT_EQ(victim_ticks.load(), 1)
      << "a target removed mid-dispatch must still receive the tick already "
         "in flight";

  // Expired only now: the snapshot released its reference when Tick returned.
  // Had it expired earlier, the delivery above would have been a
  // use-after-free.
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(coordinator.Tick(kFrameNs), 1u) << "victim is gone for good";
  EXPECT_EQ(victim_ticks.load(), 1);
}

// --- Concurrency ------------------------------------------------------------

// Bundles start and stop while the source keeps ticking. Nothing may crash,
// deadlock, or tick a target that has fully gone away.
TEST(VsyncCoordinator, ConcurrentAddRemoveDuringSustainedTicks) {
  VsyncCoordinator coordinator;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> ticks{0};

  std::thread source([&] {
    while (!stop.load()) {
      ticks += coordinator.Tick(kFrameNs);
    }
  });

  constexpr int kBundles = 16;
  std::vector<std::thread> churn;
  churn.reserve(kBundles);
  for (int i = 0; i < kBundles; ++i) {
    churn.emplace_back([&coordinator, i] {
      const std::string name = "bundle" + std::to_string(i);
      for (int round = 0; round < 50; ++round) {
        coordinator.Add(name, static_cast<Priority>(i % 3), MakeRecorder(name));
        coordinator.Remove(name);
      }
    });
  }
  for (auto& t : churn) {
    t.join();
  }
  stop = true;
  source.join();

  // Every bundle removed itself last, so the coordinator ends empty.
  EXPECT_EQ(coordinator.size(), 0u);
  EXPECT_EQ(coordinator.Tick(kFrameNs), 0u);
}

// Two sources should never exist for one display, but the coordinator must not
// corrupt its ordering if a backend double-drives it during a mode change.
TEST(VsyncCoordinator, ConcurrentTicksDeliverToEveryTarget) {
  VsyncCoordinator coordinator;
  constexpr int kTargets = 8;
  std::vector<std::shared_ptr<Recorder>> targets;
  targets.reserve(kTargets);
  for (int i = 0; i < kTargets; ++i) {
    auto r = MakeRecorder("t" + std::to_string(i));
    targets.push_back(r);
    ASSERT_TRUE(coordinator.Add("t" + std::to_string(i), Priority::kNormal, r));
  }

  constexpr int kTicksPerThread = 200;
  constexpr int kThreads = 4;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&coordinator] {
      for (int t = 0; t < kTicksPerThread; ++t) {
        coordinator.Tick(kFrameNs);
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }

  for (const auto& target : targets) {
    EXPECT_EQ(target->count(), kThreads * kTicksPerThread);
  }
}
