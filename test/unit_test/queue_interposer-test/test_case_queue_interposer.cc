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

#include <cstdint>
#include <cstring>
#include <future>
#include <mutex>

#include "gtest/gtest.h"

#include "backend/vulkan/queue_interposer.h"

using ihs::vulkan::QueueInterposer;

namespace {

template <typename T>
T Handle(const uintptr_t value) {
  return reinterpret_cast<T>(value);
}

// True when another thread cannot take @p mutex right now, i.e. it is held.
bool HeldElsewhere(std::mutex& mutex) {
  return std::async(std::launch::async,
                    [&mutex] {
                      if (!mutex.try_lock()) {
                        return true;
                      }
                      mutex.unlock();
                      return false;
                    })
      .get();
}

// Fake driver vkDeviceWaitIdle: samples the lock state of the mutexes under
// test while it "runs".
std::mutex* g_watch_a = nullptr;
std::mutex* g_watch_b = nullptr;
bool g_a_held = false;
bool g_b_held = false;
int g_calls = 0;

VKAPI_ATTR VkResult VKAPI_CALL FakeDeviceWaitIdle(VkDevice /*device*/) {
  ++g_calls;
  g_a_held = g_watch_a != nullptr && HeldElsewhere(*g_watch_a);
  g_b_held = g_watch_b != nullptr && HeldElsewhere(*g_watch_b);
  return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FakeGdpa(VkDevice /*device*/,
                                                  const char* name) {
  if (std::strcmp(name, "vkDeviceWaitIdle") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(FakeDeviceWaitIdle);
  }
  return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FakeGipa(VkInstance /*instance*/,
                                                  const char* name) {
  if (std::strcmp(name, "vkDeviceWaitIdle") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(FakeDeviceWaitIdle);
  }
  if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(FakeGdpa);
  }
  return nullptr;
}

class QueueInterposerDeviceIdle : public ::testing::Test {
 protected:
  void SetUp() override {
    g_watch_a = &mutex_a_;
    g_watch_b = &mutex_b_;
    g_a_held = g_b_held = false;
    g_calls = 0;
  }
  void TearDown() override {
    for (const uintptr_t q : {0x101u, 0x102u, 0x201u}) {
      QueueInterposer::UnregisterQueue(Handle<VkQueue>(q));
    }
    g_watch_a = g_watch_b = nullptr;
  }

  const VkDevice device_a_ = Handle<VkDevice>(0x100);
  const VkDevice device_b_ = Handle<VkDevice>(0x200);
  std::mutex mutex_a_;
  std::mutex mutex_b_;
};

TEST_F(QueueInterposerDeviceIdle, HoldsTheDevicesQueueLockOnly) {
  QueueInterposer::RegisterQueue(device_a_, Handle<VkQueue>(0x101), &mutex_a_);
  QueueInterposer::RegisterQueue(device_b_, Handle<VkQueue>(0x201), &mutex_b_);

  EXPECT_EQ(QueueInterposer::DeviceWaitIdle(device_a_, FakeDeviceWaitIdle),
            VK_SUCCESS);
  EXPECT_EQ(g_calls, 1);
  EXPECT_TRUE(g_a_held);
  EXPECT_FALSE(g_b_held);
  EXPECT_FALSE(HeldElsewhere(mutex_a_));  // released afterwards
}

TEST_F(QueueInterposerDeviceIdle, QueuesSharingOneMutexLockItOnce) {
  QueueInterposer::RegisterQueue(device_a_, Handle<VkQueue>(0x101), &mutex_a_);
  QueueInterposer::RegisterQueue(device_a_, Handle<VkQueue>(0x102), &mutex_a_);

  // A double lock of a std::mutex would deadlock here.
  EXPECT_EQ(QueueInterposer::DeviceWaitIdle(device_a_, FakeDeviceWaitIdle),
            VK_SUCCESS);
  EXPECT_TRUE(g_a_held);
}

TEST_F(QueueInterposerDeviceIdle, HoldsEveryQueueOfTheDevice) {
  QueueInterposer::RegisterQueue(device_a_, Handle<VkQueue>(0x101), &mutex_a_);
  QueueInterposer::RegisterQueue(device_a_, Handle<VkQueue>(0x102), &mutex_b_);

  QueueInterposer::DeviceWaitIdle(device_a_, FakeDeviceWaitIdle);
  EXPECT_TRUE(g_a_held);
  EXPECT_TRUE(g_b_held);
}

TEST_F(QueueInterposerDeviceIdle, UnregisteredDevicePassesThrough) {
  EXPECT_EQ(QueueInterposer::DeviceWaitIdle(device_a_, FakeDeviceWaitIdle),
            VK_SUCCESS);
  EXPECT_EQ(g_calls, 1);
  EXPECT_FALSE(g_a_held);
}

TEST_F(QueueInterposerDeviceIdle, InterposedProcTakesTheLock) {
  QueueInterposer::RegisterQueue(device_a_, Handle<VkQueue>(0x101), &mutex_a_);

  const auto proc = reinterpret_cast<PFN_vkDeviceWaitIdle>(
      QueueInterposer::Interpose(VK_NULL_HANDLE, "vkDeviceWaitIdle", FakeGipa));
  ASSERT_NE(proc, nullptr);
  EXPECT_NE(proc, &FakeDeviceWaitIdle);  // a trampoline, not the raw proc

  EXPECT_EQ(proc(device_a_), VK_SUCCESS);
  EXPECT_EQ(g_calls, 1);
  EXPECT_TRUE(g_a_held);
}

TEST_F(QueueInterposerDeviceIdle, InterposedThroughDeviceProcAddr) {
  QueueInterposer::RegisterQueue(device_a_, Handle<VkQueue>(0x101), &mutex_a_);

  // The Skia path: device procs come from a vkGetDeviceProcAddr obtained from
  // the instance callback.
  const auto gdpa =
      reinterpret_cast<PFN_vkGetDeviceProcAddr>(QueueInterposer::Interpose(
          VK_NULL_HANDLE, "vkGetDeviceProcAddr", FakeGipa));
  ASSERT_NE(gdpa, nullptr);
  const auto proc = reinterpret_cast<PFN_vkDeviceWaitIdle>(
      gdpa(device_a_, "vkDeviceWaitIdle"));
  ASSERT_NE(proc, nullptr);
  EXPECT_NE(proc, &FakeDeviceWaitIdle);

  EXPECT_EQ(proc(device_a_), VK_SUCCESS);
  EXPECT_EQ(g_calls, 1);
  EXPECT_TRUE(g_a_held);
}

}  // namespace
