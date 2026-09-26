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

#include <atomic>

#include <gtest/gtest.h>

#include "backend/common/flip_arm.h"
#include "task_runner.h"
#include "vsync/ivsync_provider.h"

namespace {

constexpr intptr_t kBaton = 0x1234;

// A real TaskRunner with a null engine, as in vsync_park-test: PostOnVsync
// bails on a null engine before posting, so the provider believes it can
// deliver and what the assertions read is whether the baton left
// vsync_baton_ -- which is the question here too.
class WiredRunner {
 public:
  WiredRunner() : runner_("flip-arm-test", engine_) {}
  TaskRunner* get() { return &runner_; }

 private:
  FLUTTER_API_SYMBOL(FlutterEngine) engine_ { nullptr };
  TaskRunner runner_;
};

// The real DrmVsyncProvider overrides IsSourcePending() to read the backend's
// own flip-in-flight latch rather than the provider's source_pending_ flag.
// Model that here, or SubmitBaton has nothing to gate on and the parking cases
// below prove nothing.
class LatchedVsync : public ivi::IVsyncProvider {
 public:
  explicit LatchedVsync(const std::atomic<bool>& latch) : latch_(latch) {}

 protected:
  [[nodiscard]] bool IsSourcePending() const override {
    return latch_.load(std::memory_order_acquire);
  }

 private:
  const std::atomic<bool>& latch_;
};

}  // namespace

// The race this class exists for: the flip reader completes the commit before
// the raster thread gets back from the ioctl. Committing must not re-arm the
// latch the reader just cleared, or the next baton parks forever (#649).
TEST(FlipArm, CommitDoesNotReArmAfterAnEarlyCompletion) {
  std::atomic<bool> latch{false};
  LatchedVsync vsync(latch);
  WiredRunner runner;
  vsync.SetEngine(nullptr, runner.get());

  ScopedFlipArm arm(latch, vsync, /*nonblocking=*/true, /*drives_vsync=*/true,
                    /*mirror_source_pending=*/false);
  EXPECT_TRUE(latch.load(std::memory_order_acquire))
      << "armed before the commit, which is the whole point";

  // The flip reader gets there first: completion clears the latch, and
  // DeliverVsync finds no baton because Flutter has not asked yet.
  latch.store(false, std::memory_order_release);
  vsync.DeliverVsync(1000);

  arm.Commit();
  EXPECT_FALSE(latch.load(std::memory_order_acquire))
      << "Commit() must not raise a latch the reader already cleared";

  // Flutter asks now. With a stale latch this baton would park forever.
  vsync.SubmitBaton(nullptr, kBaton);
  EXPECT_FALSE(vsync.HasParkedBaton())
      << "no flip is in flight, so the baton must be handed straight back";
}

// A committed arm leaves the latch up: the reader owns clearing it.
TEST(FlipArm, CommitLeavesTheLatchForTheReader) {
  std::atomic<bool> latch{false};
  LatchedVsync vsync(latch);

  {
    ScopedFlipArm arm(latch, vsync, true, true, false);
    arm.Commit();
    EXPECT_FALSE(arm.armed());
  }
  EXPECT_TRUE(latch.load(std::memory_order_acquire))
      << "the flip is in flight; only its event may clear this";
}

// The failure case that the naive fix introduces: a baton parked against the
// armed latch while the commit was in the ioctl, and then the commit fails.
TEST(FlipArm, AbortOnFailureReturnsAParkedBaton) {
  std::atomic<bool> latch{false};
  LatchedVsync vsync(latch);
  WiredRunner runner;
  vsync.SetEngine(nullptr, runner.get());

  {
    ScopedFlipArm arm(latch, vsync, true, true, false);
    vsync.SubmitBaton(nullptr, kBaton);
    EXPECT_TRUE(vsync.HasParkedBaton())
        << "the latch is up, so this parks -- that is expected";
    // scope exit without Commit(): the commit failed
  }

  EXPECT_FALSE(latch.load(std::memory_order_acquire));
  EXPECT_FALSE(vsync.HasParkedBaton())
      << "a failed commit must hand the baton back; nothing else will";
}

// Same, via an explicit Abort() ahead of a fallback path, which is how the
// call sites use it: disarm before falling back, not at scope exit.
TEST(FlipArm, ExplicitAbortIsIdempotentAndDisarmsFirst) {
  std::atomic<bool> latch{false};
  LatchedVsync vsync(latch);
  WiredRunner runner;
  vsync.SetEngine(nullptr, runner.get());

  ScopedFlipArm arm(latch, vsync, true, true, false);
  vsync.SubmitBaton(nullptr, kBaton);
  arm.Abort();
  EXPECT_FALSE(latch.load(std::memory_order_acquire));
  EXPECT_FALSE(vsync.HasParkedBaton());
  EXPECT_FALSE(arm.armed());

  // A second Abort(), and the destructor after it, must not touch the latch
  // again -- a later commit may legitimately have armed it by then.
  latch.store(true, std::memory_order_release);
  arm.Abort();
  EXPECT_TRUE(latch.load(std::memory_order_acquire))
      << "an aborted arm no longer owns the latch";
}

// No baton parked: disarming is harmless and a later request drains inline.
TEST(FlipArm, AbortWithNoBatonIsHarmless) {
  std::atomic<bool> latch{false};
  LatchedVsync vsync(latch);
  WiredRunner runner;
  vsync.SetEngine(nullptr, runner.get());

  {
    ScopedFlipArm arm(latch, vsync, true, true, false);
  }
  EXPECT_FALSE(latch.load(std::memory_order_acquire));

  vsync.SubmitBaton(nullptr, kBaton);
  EXPECT_FALSE(vsync.HasParkedBaton())
      << "latch clear, so SubmitBaton drains inline";
}

// A blocking modeset or cursor enable gets no page-flip event, so arming it
// would strand the latch with nothing to clear it.
TEST(FlipArm, BlockingCommitsAreNeverArmed) {
  std::atomic<bool> latch{false};
  LatchedVsync vsync(latch);
  WiredRunner runner;
  vsync.SetEngine(nullptr, runner.get());

  {
    ScopedFlipArm arm(latch, vsync, /*nonblocking=*/false, true, false);
    EXPECT_FALSE(arm.armed());
    EXPECT_FALSE(latch.load(std::memory_order_acquire));
    vsync.SubmitBaton(nullptr, kBaton);
  }
  EXPECT_FALSE(latch.load(std::memory_order_acquire))
      << "the latch was never this object's to touch";
}

// A secondary output's failed commit must not hand back the primary's baton:
// the primary's own flip is still in flight and will return it.
TEST(FlipArm, SecondaryOutputDoesNotDrainOnAbort) {
  std::atomic<bool> latch{false};
  LatchedVsync vsync(latch);
  WiredRunner runner;
  vsync.SetEngine(nullptr, runner.get());

  {
    ScopedFlipArm arm(latch, vsync, true, /*drives_vsync=*/false, false);
    vsync.SubmitBaton(nullptr, kBaton);
    EXPECT_TRUE(vsync.HasParkedBaton());
  }
  EXPECT_FALSE(latch.load(std::memory_order_acquire))
      << "its own latch still clears";
  EXPECT_TRUE(vsync.HasParkedBaton())
      << "but the baton belongs to whoever drives vsync";
}

// The Vulkan backend mirrors the latch into the provider's source-pending
// flag. That flag is protected, so assert what it is for: while mirrored and
// armed a baton parks, and after the abort a fresh one drains.
TEST(FlipArm, MirroredSourcePendingGatesTheBaton) {
  std::atomic<bool> latch{false};
  LatchedVsync vsync(latch);
  WiredRunner runner;
  vsync.SetEngine(nullptr, runner.get());

  {
    ScopedFlipArm arm(latch, vsync, true, true, /*mirror_source_pending=*/true);
    EXPECT_TRUE(latch.load(std::memory_order_acquire));
    vsync.SubmitBaton(nullptr, kBaton);
    EXPECT_TRUE(vsync.HasParkedBaton())
        << "source-pending is set, so the inline drain is gated off";
  }

  EXPECT_FALSE(latch.load(std::memory_order_acquire));
  EXPECT_FALSE(vsync.HasParkedBaton()) << "abort drains what it gated";

  // And source-pending really was cleared, not just the latch: a new request
  // drains inline rather than parking.
  vsync.SubmitBaton(nullptr, kBaton);
  EXPECT_FALSE(vsync.HasParkedBaton())
      << "no present in flight, so nothing should gate this";
}
