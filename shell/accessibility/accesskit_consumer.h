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

#ifndef SHELL_ACCESSIBILITY_ACCESSKIT_CONSUMER_H_
#define SHELL_ACCESSIBILITY_ACCESSKIT_CONSUMER_H_

#include <accesskit.h>

#include <atomic>
#include <cstdint>
#include <thread>

#include "ihs/ihs_semantics.h"

namespace accessibility {

// Maps a hub role onto AccessKit's. Exposed for testing: the table is pure
// translation between two independent enumerations, and a wrong entry makes a
// screen reader announce the wrong kind of control -- which is the sort of
// thing nobody notices without an assistive technology attached.
accesskit_role ToAccessKitRole(IhsSemanticsRole role);

// Maps an AccessKit action onto the hub's. Returns 0 when AccessKit asks for
// something the framework has no equivalent for, which the caller reports
// rather than dispatching a guess.
uint64_t ToIhsAction(accesskit_action action);

// Bridges the semantics hub to a platform screen reader through AccessKit.
//
// This is a hub consumer like any other: it registers, waits on the hub's
// notify fd, and reads published snapshots. It never touches the shell's
// mutable AccessibilityTree, which is the point -- AccessKit's callbacks run
// on its own AT-SPI thread, so reading the tree directly raced the platform
// thread mutating it.
//
// Actions travel the other way through ihs_semantics_dispatch. Unlike an
// automation consumer, this one *does* hold the accessibility-focus actions:
// moving the screen-reader cursor is exactly its job, and the hub's allow mask
// is what keeps other consumers from doing the same.
//
// Lifetime: construct once when semantics starts and destroy at teardown.
// Construction registers with the hub and starts the AT-SPI adapter;
// destruction unregisters, stops the polling thread, and drops the adapter, in
// that order, so no callback can be in flight afterwards.
class AccessKitConsumer {
 public:
  AccessKitConsumer();
  ~AccessKitConsumer();

  AccessKitConsumer(const AccessKitConsumer&) = delete;
  AccessKitConsumer& operator=(const AccessKitConsumer&) = delete;

  // True when the adapter and hub registration both came up. A false return
  // means the screen-reader bridge is absent, not that semantics is broken:
  // everything else still works, so the caller logs and carries on.
  [[nodiscard]] bool IsRunning() const { return running_; }

 private:
  // Drains the notify fd and pushes the newest snapshot to AccessKit. Runs on
  // its own thread so a slow AT-SPI round trip cannot delay the platform
  // thread, which is the reason the hub notifies by fd rather than callback.
  void PollLoop();

  // Publishes `snapshot` as a full AccessKit tree update.
  void PushSnapshot(const IhsSemanticsSnapshot* snapshot);

  IhsSemanticsConsumer* consumer_ = nullptr;
  int notify_fd_ = -1;    // eventfd the hub writes on publish
  int shutdown_fd_ = -1;  // eventfd the destructor writes to stop the loop
  std::thread poll_thread_;
  std::atomic<bool> running_{false};
  // Last generation pushed, so a wake that coalesced several publications
  // still results in exactly one update and a redundant wake results in none.
  uint64_t last_generation_ = 0;
};

}  // namespace accessibility

#endif  // SHELL_ACCESSIBILITY_ACCESSKIT_CONSUMER_H_
