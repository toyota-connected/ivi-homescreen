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

// Unit tests for MainLoopWaker.
//
// MainLoopWaker is a process-wide singleton backed by a real eventfd.  The
// tests exercise Wake(), Wait(), SignalWake(), and fd() using the same
// poll()-based approach that wake_event_fd-test uses, so there is no
// dependency on Wayland or the Flutter engine.
//
// NOTE: Because the singleton is initialised once per process all tests run
// in the same waker instance.  Each test drains any pending tokens with a
// short Wait() before proceeding so they are independent of one another.

#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "main_loop_waker.h"

using namespace std::chrono_literals;

namespace {

// Poll the fd for POLLIN; returns the revents or 0 on timeout.
short PollIn(const int fd, const int timeout_ms) {
  pollfd p{fd, POLLIN, 0};
  const int r = ::poll(&p, 1, timeout_ms);
  return r > 0 ? p.revents : short{0};
}

// Drain any pending wake tokens so the next test starts clean.
void Drain() {
  MainLoopWaker::instance().Wait(/*timeout_ms=*/1);
}

}  // namespace

class MainLoopWakerTest : public ::testing::Test {
 protected:
  void SetUp() override { Drain(); }
};

// ---- fd validity ----------------------------------------------------------

TEST_F(MainLoopWakerTest, FdIsValid) {
  EXPECT_GE(MainLoopWaker::instance().fd(), 0);
}

// ---- Wake makes fd readable -----------------------------------------------

TEST_F(MainLoopWakerTest, WakeMakesFdReadable) {
  MainLoopWaker::instance().Wake();
  const short revents = PollIn(MainLoopWaker::instance().fd(), /*timeout_ms=*/50);
  EXPECT_NE(revents & POLLIN, 0);
  Drain();
}

// ---- SignalWake makes fd readable -----------------------------------------

TEST_F(MainLoopWakerTest, SignalWakeMakesFdReadable) {
  MainLoopWaker::SignalWake();
  const short revents = PollIn(MainLoopWaker::instance().fd(), /*timeout_ms=*/50);
  EXPECT_NE(revents & POLLIN, 0);
  Drain();
}

// ---- Wait returns after Wake ----------------------------------------------

TEST_F(MainLoopWakerTest, WaitReturnsAfterWake) {
  // A background thread wakes the main loop after 10 ms.
  std::thread waker([] {
    std::this_thread::sleep_for(10ms);
    MainLoopWaker::instance().Wake();
  });

  const auto t0 = std::chrono::steady_clock::now();
  MainLoopWaker::instance().Wait(/*timeout_ms=*/500);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0)
          .count();

  waker.join();

  // Must have returned well before the 500 ms timeout.
  EXPECT_LT(elapsed, 400);
}

// ---- Wait returns on timeout ----------------------------------------------

TEST_F(MainLoopWakerTest, WaitReturnsOnTimeout) {
  const auto t0 = std::chrono::steady_clock::now();
  MainLoopWaker::instance().Wait(/*timeout_ms=*/20);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0)
          .count();

  // Should return within a generous bound (CI can be slow).
  EXPECT_GE(elapsed, 15);
  EXPECT_LT(elapsed, 500);
}

// ---- Wait drains: a second Wait without Wake blocks -----------------------

TEST_F(MainLoopWakerTest, WaitDrainsToken) {
  MainLoopWaker::instance().Wake();
  // First Wait: consumes the token.
  MainLoopWaker::instance().Wait(/*timeout_ms=*/50);
  // Second Wait: no token, must time out quickly.
  const auto t0 = std::chrono::steady_clock::now();
  MainLoopWaker::instance().Wait(/*timeout_ms=*/20);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0)
          .count();
  EXPECT_GE(elapsed, 15);
}

// ---- Multiple Wake() calls coalesce into one token ------------------------

TEST_F(MainLoopWakerTest, MultipleWakesCoalesceIntoOneToken) {
  // Three Wake() calls coalesce because eventfd adds to the counter and a
  // single nonblocking read drains it entirely.  After one Wait() the fd must
  // be non-readable (i.e. the second Wait() times out).
  MainLoopWaker::instance().Wake();
  MainLoopWaker::instance().Wake();
  MainLoopWaker::instance().Wake();

  MainLoopWaker::instance().Wait(/*timeout_ms=*/50);

  // fd should now be empty.
  const short revents = PollIn(MainLoopWaker::instance().fd(), /*timeout_ms=*/5);
  EXPECT_EQ(revents & POLLIN, 0);
}
