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

#include "gtest/gtest.h"

#include "stats.h"

// getSelfStats reads /proc/self/stat which is always present on Linux.
// All four assertions must hold for any running process.

TEST(Stats, GetSelfStats_ThreadsAtLeastOne) {
  Stats::ProcessStats s{};
  Stats::getSelfStats(s);
  EXPECT_GE(s.num_threads, 1L);
}

TEST(Stats, GetSelfStats_VirtualMemoryPositive) {
  Stats::ProcessStats s{};
  Stats::getSelfStats(s);
  EXPECT_GT(s.virtual_memory, 0.0);
}

TEST(Stats, GetSelfStats_RssPositive) {
  Stats::ProcessStats s{};
  Stats::getSelfStats(s);
  EXPECT_GT(s.resident_set_size, 0.0);
}

// Virtual address space is always at least as large as the resident set.
TEST(Stats, GetSelfStats_VirtualExceedsOrEqualsRss) {
  Stats::ProcessStats s{};
  Stats::getSelfStats(s);
  EXPECT_GE(s.virtual_memory, s.resident_set_size);
}
