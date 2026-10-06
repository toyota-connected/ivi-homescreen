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

#pragma once

#include <mutex>
#include <vector>

// Signals an eventfd once a fence fd becomes readable: a compositor's release
// fence (a sync_file) passed on to a producer that waits on an eventfd it was
// handed before the fence existed.
//
// One thread serves every view. It is started on first use and never joined:
// a joinable thread in static storage would make process exit wait on it.
class FenceRelay {
 public:
  // The process's relay.
  static FenceRelay& Get();

  // Write 1 to @eventfd when @fence is readable, then close both. Takes
  // ownership of both fds on every path. A fence that reports an error counts
  // as signaled, so a producer is never left waiting on a dead one.
  void Add(int fence, int eventfd);

  // Pairs not yet signaled.
  [[nodiscard]] size_t pending() const;

  FenceRelay(const FenceRelay&) = delete;
  FenceRelay& operator=(const FenceRelay&) = delete;

 private:
  FenceRelay();
  void Run();

  struct Pair {
    int fence;
    int eventfd;
  };
  mutable std::mutex mutex_;
  std::vector<Pair> pairs_;
  // Wakes the thread when a pair is added.
  int wake_{-1};
};
