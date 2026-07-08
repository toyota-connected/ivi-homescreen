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

#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "gtest/gtest.h"

#include "input/wake_event_fd.h"

using homescreen::input::WakeEventFd;
using namespace std::chrono_literals;

namespace {

// Poll a single fd for POLLIN; returns the revents (0 on timeout). A negative
// timeout blocks indefinitely.
short PollIn(const int fd, const int timeout_ms) {
  pollfd p{fd, POLLIN, 0};
  const int r = ::poll(&p, 1, timeout_ms);
  return r > 0 ? p.revents : short{0};
}

// voluntary_ctxt_switches for a thread, read from procfs. Each time the thread
// blocks (in poll) and is descheduled it increments; this is the exact counter
// issue #261's idle-wakeup budget is measured against. -1 if unreadable.
long VoluntaryCtxtSwitches(const pid_t tid) {
  std::ifstream f("/proc/self/task/" + std::to_string(tid) + "/status");
  std::string line;
  while (std::getline(f, line)) {
    static constexpr char kKey[] = "voluntary_ctxt_switches:";
    if (line.rfind(kKey, 0) == 0) {
      return std::stol(line.substr(sizeof(kKey) - 1));
    }
  }
  return -1;
}

}  // namespace

// Wake() before the consumer blocks must still be observed: the fd is left
// readable, so the subsequent poll returns POLLIN immediately (generous timeout
// guards against a hang if the invariant is broken).
TEST(WakeEventFd, WakeBeforeWaitIsObserved) {
  const WakeEventFd w;
  ASSERT_GE(w.fd(), 0);

  w.Wake();
  EXPECT_TRUE((PollIn(w.fd(), 1000) & POLLIN) != 0);
}

// N wakes between drains coalesce into a single readable state; one Drain()
// clears it and the next non-blocking poll sees nothing.
TEST(WakeEventFd, WakesCoalesceAndDrainClears) {
  const WakeEventFd w;
  ASSERT_GE(w.fd(), 0);

  for (int i = 0; i < 1000; ++i) {
    w.Wake();
  }
  EXPECT_TRUE((PollIn(w.fd(), 0) & POLLIN) != 0);

  w.Drain();
  EXPECT_EQ(PollIn(w.fd(), 0), 0);
}

// Drain() on an unsignaled fd must not block and must leave the fd usable.
TEST(WakeEventFd, DrainEmptyIsNonBlockingNoOp) {
  const WakeEventFd w;
  ASSERT_GE(w.fd(), 0);

  w.Drain();  // nothing pending — must return immediately (EAGAIN path)
  EXPECT_EQ(PollIn(w.fd(), 0), 0);

  // Still functional afterward.
  w.Wake();
  EXPECT_TRUE((PollIn(w.fd(), 0) & POLLIN) != 0);
}

// A thread blocked in poll(-1) is released by a Wake() from another thread.
TEST(WakeEventFd, CrossThreadWakeUnblocksPoll) {
  const WakeEventFd w;
  ASSERT_GE(w.fd(), 0);

  std::atomic<bool> woke{false};
  std::thread blocker([&] {
    // 5 s upper bound: the wake is expected in microseconds; the bound only
    // prevents a hung test if the wake is lost.
    woke.store((PollIn(w.fd(), 5000) & POLLIN) != 0);
  });

  std::this_thread::sleep_for(50ms);  // let the blocker reach poll()
  const auto start = std::chrono::steady_clock::now();
  w.Wake();
  blocker.join();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_TRUE(woke.load());
  EXPECT_LT(elapsed, 2s);
}

// eventfd() failure (EMFILE) yields fd() == -1 and Wake()/Drain() are safe
// no-ops. Exercised in a child with a shrunk RLIMIT_NOFILE so the fd-exhaustion
// does not leak into the rest of the suite.
TEST(WakeEventFd, DegradedConstructionOnFdExhaustion) {
  const pid_t pid = fork();
  ASSERT_GE(pid, 0) << "fork failed";

  if (pid == 0) {
    // Child: fds 0/1/2 stay open; a soft limit of 3 forces any new fd
    // allocation (>= 3) to fail with EMFILE.
    const rlimit rl{3, 3};
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
      _exit(2);
    }
    const WakeEventFd w;
    const bool degraded = w.fd() < 0;
    w.Wake();   // must not crash in degraded mode
    w.Drain();  // must not crash in degraded mode
    _exit(degraded ? 0 : 1);
  }

  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0)
      << "child expected fd()==-1 and safe no-op Wake()/Drain()";
}

// Models the seat dispatch loop's wake contract: the loop blocks in poll(-1)
// over {input fd, waker}; real input wakes it and is processed; setting stop
// then Wake() (store -> wake -> poll-return -> drain -> re-check) breaks it out
// with no input traffic. This is the exact ordering both seats rely on.
TEST(WakeEventFd, MockDispatchLoopStopAndInput) {
  const WakeEventFd waker;
  ASSERT_GE(waker.fd(), 0);

  int input_pipe[2];
  ASSERT_EQ(pipe(input_pipe), 0);
  // Non-blocking read end so the drain-to-empty loop below EAGAIN-exits back to
  // poll() — mirrors the seats' non-blocking libinput fd. A blocking fd would
  // wedge the loop inside read() after the last byte, never re-checking stop_.
  ASSERT_EQ(fcntl(input_pipe[0], F_SETFL, O_NONBLOCK), 0);

  std::atomic<bool> stop{false};
  std::atomic<int> input_seen{0};

  std::thread loop([&] {
    while (!stop.load(std::memory_order_acquire)) {
      pollfd pfds[2] = {{input_pipe[0], POLLIN, 0}, {waker.fd(), POLLIN, 0}};
      if (::poll(pfds, 2, -1) < 0) {
        continue;
      }
      if ((pfds[1].revents & POLLIN) != 0) {
        waker.Drain();
        continue;  // re-check stop_ at the top
      }
      if ((pfds[0].revents & POLLIN) != 0) {
        char b = 0;
        while (read(input_pipe[0], &b, 1) == 1) {
          input_seen.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
  });

  // Input path: a byte on the pipe wakes poll() directly (no waker needed).
  const char c = 'x';
  ASSERT_EQ(write(input_pipe[1], &c, 1), 1);
  for (int i = 0; i < 500 && input_seen.load() == 0; ++i) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_EQ(input_seen.load(), 1);

  // Stop path: with no input pending, only the waker can break poll(-1).
  stop.store(true, std::memory_order_release);
  waker.Wake();
  loop.join();  // hangs (caught by ctest timeout) if the stop-wake is lost

  close(input_pipe[0]);
  close(input_pipe[1]);
}

// The point of #261: an idle dispatch thread blocked in poll(-1) accrues
// (essentially) zero context switches, whereas the old timed poll wakes once
// per timeout. Measures voluntary_ctxt_switches over an idle window for both
// patterns and asserts the event-driven one is far cheaper — the same counter
// and metric the I2 integration assertion uses, exercised here at the
// mechanism level without libinput or the engine.
TEST(WakeEventFd, IdleWakeupBudgetBeatsTimedPoll) {
  constexpr auto kWindow = 300ms;
  constexpr int kTimedPollMs = 16;  // the old SoftwareSeat cap

  // Run a poll loop over `waker` for the window with the given timeout, and
  // return the thread's voluntary_ctxt_switches accrued strictly inside the
  // window (measured after the thread has settled into its first block).
  const auto measure = [kWindow](const WakeEventFd& waker,
                                 int timeout_ms) -> long {
    std::atomic<pid_t> tid{-1};
    std::atomic<bool> stop{false};
    std::thread t([&] {
      tid.store(static_cast<pid_t>(syscall(SYS_gettid)));
      while (!stop.load(std::memory_order_acquire)) {
        pollfd p{waker.fd(), POLLIN, 0};
        if (::poll(&p, 1, timeout_ms) > 0 && (p.revents & POLLIN) != 0) {
          waker.Drain();
        }
      }
    });

    while (tid.load() < 0) {
      std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(20ms);  // let it reach its first block
    const long before = VoluntaryCtxtSwitches(tid.load());
    std::this_thread::sleep_for(kWindow);
    const long after = VoluntaryCtxtSwitches(tid.load());

    stop.store(true, std::memory_order_release);
    waker.Wake();
    t.join();
    EXPECT_GE(before, 0);
    EXPECT_GE(after, 0);
    return after - before;
  };

  long idle_switches = -1;
  long timed_switches = -1;
  {
    const WakeEventFd waker;
    ASSERT_GE(waker.fd(), 0);
    idle_switches = measure(waker, -1);
  }
  {
    const WakeEventFd waker;
    ASSERT_GE(waker.fd(), 0);
    timed_switches = measure(waker, kTimedPollMs);
  }

  // ~1 (the block entered before the window, no wakeups during it) vs ~300/16
  // ≈ 18. Generous bounds absorb scheduler noise; the ratio is the point.
  std::cerr << "[   METRIC ] idle_ctxt_switches=" << idle_switches
            << " timed_ctxt_switches=" << timed_switches << " over "
            << kWindow.count() << "ms idle\n";
  EXPECT_LE(idle_switches, 3);
  EXPECT_GE(timed_switches, 10);
  EXPECT_LT(idle_switches, timed_switches);
}
