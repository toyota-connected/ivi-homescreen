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

#include "backend/backing_store_pool.h"
#include "gtest/gtest.h"

namespace {

struct FakeStore {
  static std::atomic<int> alive;
  static std::atomic<int> allocated;

  FakeStore(int32_t w, int32_t h) : w_(w), h_(h) {
    ++alive;
    ++allocated;
  }
  ~FakeStore() { --alive; }

  [[nodiscard]] int32_t Width() const { return w_; }
  [[nodiscard]] int32_t Height() const { return h_; }

  int32_t w_;
  int32_t h_;
};

std::atomic<int> FakeStore::alive{0};
std::atomic<int> FakeStore::allocated{0};

class BackingStorePoolTest : public ::testing::Test {
 protected:
  void SetUp() override {
    FakeStore::alive = 0;
    FakeStore::allocated = 0;
  }
};

}  // namespace

TEST_F(BackingStorePoolTest, AcquireAllocatesWhenEmpty) {
  BackingStorePool<FakeStore> pool;
  auto a = pool.Acquire(100, 200);
  auto b = pool.Acquire(300, 400);
  EXPECT_EQ(FakeStore::allocated.load(), 2);
  EXPECT_EQ(a->Width(), 100);
  EXPECT_EQ(a->Height(), 200);
  EXPECT_EQ(b->Width(), 300);
  EXPECT_EQ(b->Height(), 400);
}

TEST_F(BackingStorePoolTest, ReleaseReusesMatchingSize) {
  BackingStorePool<FakeStore> pool;
  auto a = pool.Acquire(640, 480);
  FakeStore* raw = a.get();
  pool.Release(std::move(a));

  auto b = pool.Acquire(640, 480);
  EXPECT_EQ(b.get(), raw) << "Pool must reuse store with matching dimensions";
  EXPECT_EQ(FakeStore::allocated.load(), 1);
}

TEST_F(BackingStorePoolTest, DifferentSizesDoNotReuse) {
  BackingStorePool<FakeStore> pool;
  auto a = pool.Acquire(640, 480);
  pool.Release(std::move(a));

  auto b = pool.Acquire(800, 600);
  EXPECT_EQ(FakeStore::allocated.load(), 2);
  EXPECT_EQ(b->Width(), 800);
  EXPECT_EQ(b->Height(), 600);
}

TEST_F(BackingStorePoolTest, FlushDestroysAllPooledStores) {
  BackingStorePool<FakeStore> pool;
  pool.Release(pool.Acquire(1, 1));
  pool.Release(pool.Acquire(2, 2));
  EXPECT_EQ(pool.Size(), 2u);
  EXPECT_EQ(FakeStore::alive.load(), 2);

  pool.Flush();
  EXPECT_EQ(pool.Size(), 0u);
  EXPECT_EQ(FakeStore::alive.load(), 0);
}

TEST_F(BackingStorePoolTest, CapacityBoundsFreeList) {
  BackingStorePool<FakeStore> pool;
  pool.SetCapacity(1);
  pool.Release(pool.Acquire(1, 1));
  pool.Release(pool.Acquire(2, 2));  // dropped (capacity hit)
  EXPECT_EQ(pool.Size(), 1u);
}

TEST_F(BackingStorePoolTest, ReleaseNullIsNoop) {
  BackingStorePool<FakeStore> pool;
  pool.Release(nullptr);
  EXPECT_EQ(pool.Size(), 0u);
}

TEST_F(BackingStorePoolTest, DestructorFlushes) {
  {
    BackingStorePool<FakeStore> pool;
    pool.Release(pool.Acquire(10, 10));
    EXPECT_EQ(FakeStore::alive.load(), 1);
  }
  EXPECT_EQ(FakeStore::alive.load(), 0);
}
