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

#ifndef SHELL_MAIN_LOOP_WAKER_H_
#define SHELL_MAIN_LOOP_WAKER_H_

/**
 * @brief Process-wide wakeup channel for the main (App::Loop) thread.
 *
 * The main loop only has to do work when (a) an input thread coalesces a
 * pointer event, (b) a periodic consumer (Wayland key-repeat timer or a
 * compositor-surface plugin) needs a tick, or (c) shutdown was requested.
 * Rather than spinning at the display refresh rate, the loop blocks in
 * Wait() and is woken explicitly via Wake().
 *
 * Backed by an eventfd: Wake() is a single write() and is therefore
 * async-signal-safe, so the shutdown signal handler can use SignalWake()
 * to break the loop out of an idle block without touching locks.
 */
class MainLoopWaker {
 public:
  static MainLoopWaker& instance();

  MainLoopWaker(const MainLoopWaker&) = delete;
  MainLoopWaker& operator=(const MainLoopWaker&) = delete;

  /// Wake the main loop. Safe to call from any thread.
  void Wake() const;

  /// Block the calling (main) thread until Wake() is called or @p timeout_ms
  /// elapses. A negative timeout blocks indefinitely. Drains any pending
  /// wake tokens before returning.
  void Wait(int timeout_ms) const;

  /// Async-signal-safe wake for use from a signal handler. Writes directly
  /// to the (already-created) eventfd; does no allocation or locking.
  static void SignalWake();

 private:
  MainLoopWaker();
  ~MainLoopWaker();

  int fd_;
};

#endif  // SHELL_MAIN_LOOP_WAKER_H_
