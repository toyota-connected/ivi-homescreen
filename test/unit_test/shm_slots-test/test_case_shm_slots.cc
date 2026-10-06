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

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

#include <linux/dma-buf.h>

#include <gtest/gtest.h>

#include "fence_relay.h"
#include "shm_slots.h"

namespace {

// The driver behind GBM is not instrumented: under a sanitizer its own
// threads report races, and the device the slots keep for the process reads
// as a leak. The cases that allocate skip there; the rest still run.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SHM_SLOTS_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SHM_SLOTS_SANITIZED 1
#endif
#ifdef SHM_SLOTS_SANITIZED
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif

constexpr uint32_t kXrgb8888 = 0x34325258;  // 'XR24'
constexpr uint32_t kRgb565 = 0x36314752;    // 'RG16'

// The first render node, as a dev_t; 0 when there is none, or under a
// sanitizer (see kSanitized).
uint64_t FirstRenderNode() {
  if (kSanitized) {
    return 0;
  }
  DIR* const d = ::opendir("/dev/dri");
  if (d == nullptr) {
    return 0;
  }
  uint64_t dev = 0;
  while (const dirent* e = ::readdir(d)) {
    if (std::strncmp(e->d_name, "renderD", 7) != 0) {
      continue;
    }
    struct stat st{};
    const std::string path = std::string("/dev/dri/") + e->d_name;
    if (::stat(path.c_str(), &st) == 0 &&
        ::access(path.c_str(), R_OK | W_OK) == 0) {
      dev = static_cast<uint64_t>(st.st_rdev);
      break;
    }
  }
  ::closedir(d);
  return dev;
}

int Sync(const int fd, const uint64_t flags) {
  dma_buf_sync s{};
  s.flags = flags;
  int rc;
  do {
    rc = ::ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
  } while (rc != 0 && (errno == EINTR || errno == EAGAIN));
  return rc;
}

// Whether @fd becomes readable within @ms.
bool Readable(const int fd, const int ms) {
  pollfd p{fd, POLLIN, 0};
  return ::poll(&p, 1, ms) == 1 && (p.revents & POLLIN) != 0;
}

}  // namespace

TEST(ShmSlots, KnowsTheFormatsASlotHolds) {
  EXPECT_EQ(ShmSlots::BytesPerPixel(kXrgb8888), 4u);
  EXPECT_EQ(ShmSlots::BytesPerPixel(kRgb565), 2u);
  EXPECT_EQ(ShmSlots::BytesPerPixel(0x3231564e /* NV12 */), 0u);
}

TEST(ShmSlots, RefusesWhatCannotBeAllocated) {
  EXPECT_EQ(ShmSlots::Allocate(0, 64, 64, kXrgb8888), nullptr);
  EXPECT_EQ(ShmSlots::Allocate(FirstRenderNode(), 0, 64, kXrgb8888), nullptr);
  EXPECT_EQ(ShmSlots::Allocate(FirstRenderNode(), 64, 64, 0x3231564e), nullptr);
}

// The producer's side of the contract: each slot maps read-write, takes a
// bracketed write, and reads it back through the same mapping. Two slots, one
// stride, distinct buffers.
TEST(ShmSlots, SlotsAreDistinctWritableDmabufs) {
  const uint64_t node = FirstRenderNode();
  if (node == 0) {
    GTEST_SKIP() << "no accessible render node, or a sanitizer build";
  }
  const auto slots = ShmSlots::Allocate(node, 96, 40, kXrgb8888);
  if (slots == nullptr) {
    GTEST_SKIP() << "no GBM allocation on this machine";
  }
  ASSERT_EQ(ShmSlots::kCount, 2u);
  EXPECT_EQ(slots->width(), 96u);
  EXPECT_EQ(slots->height(), 40u);
  EXPECT_EQ(slots->fourcc(), kXrgb8888);
  EXPECT_GE(slots->stride(), 96u * 4);
  EXPECT_NE(slots->fd(0), slots->fd(1));

  for (uint32_t i = 0; i < ShmSlots::kCount; ++i) {
    const int fd = slots->fd(i);
    ASSERT_GE(fd, 0);
    EXPECT_NE(::fcntl(fd, F_GETFD) & FD_CLOEXEC, 0);
    // Room for the padding row as well as the image.
    const off_t size = ::lseek(fd, 0, SEEK_END);
    EXPECT_GE(size, static_cast<off_t>(slots->stride()) * 41);
    const size_t bytes = static_cast<size_t>(slots->stride()) * 41;
    void* map =
        ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_NE(map, MAP_FAILED) << "slot " << i << " is not writable";
    ASSERT_EQ(Sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE), 0);
    auto* px = static_cast<uint32_t*>(map);
    px[0] = 0xff102030u + i;
    ASSERT_EQ(Sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE), 0);
    EXPECT_EQ(px[0], 0xff102030u + i);
    ::munmap(map, bytes);
  }
}

// The submit check: the slot's own fd and any dup of it match that index, and
// nothing else does.
TEST(ShmSlots, MatchesOnlyTheSlotsOwnBuffer) {
  const uint64_t node = FirstRenderNode();
  if (node == 0) {
    GTEST_SKIP() << "no accessible render node, or a sanitizer build";
  }
  const auto slots = ShmSlots::Allocate(node, 32, 32, kXrgb8888);
  const auto other = ShmSlots::Allocate(node, 32, 32, kXrgb8888);
  if (slots == nullptr || other == nullptr) {
    GTEST_SKIP() << "no GBM allocation on this machine";
  }
  const int dup0 = ::dup(slots->fd(0));
  ASSERT_GE(dup0, 0);
  EXPECT_TRUE(slots->Matches(0, slots->fd(0)));
  EXPECT_TRUE(slots->Matches(0, dup0));
  EXPECT_TRUE(slots->Matches(1, slots->fd(1)));
  EXPECT_FALSE(slots->Matches(1, dup0)) << "slot 0 submitted as slot 1";
  EXPECT_FALSE(slots->Matches(0, other->fd(0))) << "another grant's buffer";
  EXPECT_FALSE(slots->Matches(2, slots->fd(0))) << "no such slot";
  EXPECT_FALSE(slots->Matches(0, -1));
  ::close(dup0);

  const int unrelated = ::eventfd(0, EFD_CLOEXEC);
  EXPECT_FALSE(slots->Matches(0, unrelated));
  ::close(unrelated);
}

// The relay fires the eventfd only once the fence does, and closes both.
TEST(FenceRelay, SignalsWhenTheFenceDoes) {
  const int fence = ::eventfd(0, EFD_CLOEXEC);
  const int release = ::eventfd(0, EFD_CLOEXEC);
  const int producer = ::dup(release);  // what the producer waits on
  ASSERT_GE(fence, 0);
  ASSERT_GE(producer, 0);
  const int fence_writer = ::dup(fence);

  FenceRelay::Get().Add(fence, release);
  EXPECT_FALSE(Readable(producer, 50)) << "released before the fence fired";

  ASSERT_EQ(eventfd_write(fence_writer, 1), 0);
  EXPECT_TRUE(Readable(producer, 2000));
  ::close(fence_writer);
  ::close(producer);

  // Both of the relay's fds were closed once it fired.
  for (int i = 0; i < 100 && FenceRelay::Get().pending() != 0; ++i) {
    ::usleep(1000);
  }
  EXPECT_EQ(FenceRelay::Get().pending(), 0u);
}

// No fence to wait on means no wait.
TEST(FenceRelay, SignalsAtOnceWithoutAFence) {
  const int release = ::eventfd(0, EFD_CLOEXEC);
  const int producer = ::dup(release);
  FenceRelay::Get().Add(-1, release);
  EXPECT_TRUE(Readable(producer, 0));
  ::close(producer);
}

// Many pairs, fired out of order: each producer wakes exactly when its own
// fence does.
TEST(FenceRelay, KeepsPairsApart) {
  constexpr int kPairs = 8;
  int fences[kPairs];
  int producers[kPairs];
  for (int i = 0; i < kPairs; ++i) {
    const int fence = ::eventfd(0, EFD_CLOEXEC);
    const int release = ::eventfd(0, EFD_CLOEXEC);
    fences[i] = ::dup(fence);
    producers[i] = ::dup(release);
    FenceRelay::Get().Add(fence, release);
  }
  for (int i = kPairs - 1; i >= 0; i -= 2) {
    ASSERT_EQ(eventfd_write(fences[i], 1), 0);
  }
  for (int i = 0; i < kPairs; ++i) {
    const bool fired = (i % 2) == 1;
    EXPECT_EQ(Readable(producers[i], fired ? 2000 : 20), fired) << "pair " << i;
  }
  for (int i = 0; i < kPairs; i += 2) {
    ASSERT_EQ(eventfd_write(fences[i], 1), 0);
    EXPECT_TRUE(Readable(producers[i], 2000)) << "pair " << i;
  }
  for (int i = 0; i < kPairs; ++i) {
    ::close(fences[i]);
    ::close(producers[i]);
  }
}
