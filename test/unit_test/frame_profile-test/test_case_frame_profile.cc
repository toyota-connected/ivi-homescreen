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

#include <cstdlib>
#include <string_view>

#include "gtest/gtest.h"

#include "profiling/frame_profile.h"

using profiling::FrameProfile;

// ---------------------------------------------------------------------------
// Enabled() — env-var gate
// ---------------------------------------------------------------------------

// Temporarily clear IVI_PROFILE for the duration of a test.
struct EnvGuard {
  explicit EnvGuard(const char* name) : name_(name) {
    saved_ = std::getenv(name);
    ::unsetenv(name);
  }
  ~EnvGuard() {
    if (saved_) {
      ::setenv(name_, saved_, 1);
    } else {
      ::unsetenv(name_);
    }
  }
  const char* name_;
  const char* saved_{nullptr};
};

TEST(FrameProfile, Enabled_WhenNoEnvSet) {
  EnvGuard g1("IVI_PROFILE");
  EnvGuard g2("MY_LEGACY_GATE");
  EXPECT_FALSE(FrameProfile::Enabled());
  EXPECT_FALSE(FrameProfile::Enabled("MY_LEGACY_GATE"));
}

TEST(FrameProfile, Enabled_WhenIviProfileSet) {
  ::setenv("IVI_PROFILE", "1", 1);
  EXPECT_TRUE(FrameProfile::Enabled());
  EXPECT_TRUE(FrameProfile::Enabled("IGNORED_LEGACY"));
  ::unsetenv("IVI_PROFILE");
}

TEST(FrameProfile, Enabled_LegacyEnvGate) {
  EnvGuard g("IVI_PROFILE");  // ensure IVI_PROFILE is clear
  ::setenv("MY_LEGACY_GATE", "1", 1);
  EXPECT_TRUE(FrameProfile::Enabled("MY_LEGACY_GATE"));
  EXPECT_FALSE(FrameProfile::Enabled("OTHER_GATE"));
  ::unsetenv("MY_LEGACY_GATE");
}

// ---------------------------------------------------------------------------
// Record() — no crash, discarded / stalled flags, explicit timestamps
// ---------------------------------------------------------------------------

TEST(FrameProfile, Record_DiscardedFrame_DoesNotCrash) {
  FrameProfile fp;
  fp.Record("test", /*ok=*/false, /*now_ns=*/1'000'000ULL);
}

TEST(FrameProfile, Record_OkFrame_DoesNotCrash) {
  FrameProfile fp;
  // Two frames 16 ms apart so there's an inter-frame interval.
  fp.Record("test", true, 0ULL);
  fp.Record("test", true, 16'000'000ULL);
}

TEST(FrameProfile, Record_StalledFrame_DoesNotCrash) {
  FrameProfile fp;
  fp.Record("test", true, 0ULL, /*stalled=*/true);
}

TEST(FrameProfile, Record_MixedOkAndDiscarded_DoesNotCrash) {
  FrameProfile fp;
  for (int i = 0; i < 10; ++i) {
    fp.Record("test", /*ok=*/(i % 2 == 0),
              static_cast<uint64_t>(i) * 16'000'000ULL);
  }
}

// ---------------------------------------------------------------------------
// Record() — window flush at kWindow (60) frames
// ---------------------------------------------------------------------------

TEST(FrameProfile, Record_WindowFlush_60Frames_DoesNotCrash) {
  FrameProfile fp;
  // Feed 60 frames at 16 ms intervals (≈ 60 Hz); the 60th call triggers the
  // window flush and merges into the session stats.
  for (int i = 0; i < 60; ++i) {
    fp.Record("test", true, static_cast<uint64_t>(i) * 16'000'000ULL);
  }
  // One more frame after the flush: exercises the post-reset code path.
  fp.Record("test", true, 60ULL * 16'000'000ULL);
}

TEST(FrameProfile, Record_MultipleWindowFlushes_DoesNotCrash) {
  FrameProfile fp;
  for (int i = 0; i < 180; ++i) {
    fp.Record("test", true, static_cast<uint64_t>(i) * 16'000'000ULL);
  }
}

// ---------------------------------------------------------------------------
// LogSessionSummary() — empty and populated
// ---------------------------------------------------------------------------

TEST(FrameProfile, LogSessionSummary_EmptyProfile_DoesNotCrash) {
  const FrameProfile fp;
  fp.LogSessionSummary("empty");
}

TEST(FrameProfile, LogSessionSummary_AfterSomeRecords_DoesNotCrash) {
  FrameProfile fp;
  for (int i = 0; i < 5; ++i) {
    fp.Record("test", true, static_cast<uint64_t>(i) * 16'000'000ULL);
  }
  fp.LogSessionSummary("partial");
}

TEST(FrameProfile, LogSessionSummary_AfterFullWindow_DoesNotCrash) {
  FrameProfile fp;
  for (int i = 0; i < 65; ++i) {
    fp.Record("test", true, static_cast<uint64_t>(i) * 16'000'000ULL);
  }
  fp.LogSessionSummary("full-window");
}

// ---------------------------------------------------------------------------
// Record() — bucket boundary values
// All intervals fed through Record() with explicit now_ns; the bucket
// classification (<=17ms/<=33ms/<=50ms/<=100ms/>100ms) is exercised inside
// Stats::AddInterval which is called via Record when ok=true.
// ---------------------------------------------------------------------------

TEST(FrameProfile, Record_AllBucketBoundaries_DoesNotCrash) {
  FrameProfile fp;
  // Use pairs (t0, t1) to produce the desired inter-frame interval dt = t1-t0.
  // 60 Hz bucket: dt <= 17 ms
  fp.Record("b", true, 0ULL);
  fp.Record("b", true, 17'000'000ULL);  // dt = 17 ms → b60
  // 30 Hz bucket: 17 < dt <= 33 ms
  fp.Record("b", true, 17'000'000ULL + 33'000'000ULL);  // dt = 33 ms → b30
  // 20 Hz bucket: 33 < dt <= 50 ms
  fp.Record("b", true,
            17'000'000ULL + 33'000'000ULL + 50'000'000ULL);  // dt = 50 ms → b20
  // slow bucket: 50 < dt <= 100 ms
  fp.Record("b", true,
            17'000'000ULL + 33'000'000ULL + 50'000'000ULL +
                100'000'000ULL);  // dt = 100 ms → bslow
  // idle bucket: dt > 100 ms
  fp.Record("b", true,
            17'000'000ULL + 33'000'000ULL + 50'000'000ULL + 100'000'000ULL +
                101'000'000ULL);  // dt = 101 ms → bidle
  fp.LogSessionSummary("buckets");
}
