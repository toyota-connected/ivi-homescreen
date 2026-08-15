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

#include <string>

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

// Translates one hub node into an AccessKit node. Exposed for testing: several
// of the rules here are invisible without an assistive technology attached and
// fail silently when wrong -- a Label's accessible name comes from its value
// rather than its label, and the focus action must be offered only where the
// node can actually hold the accessibility cursor.
//
// `snapshot` resolves the node's children and custom-action declarations, so
// it must be the snapshot the node came from. Caller owns the result.
/*
 * `slot` is the application this node belongs to; child ids are composed from
 * it so two applications' node 1 do not collide in AccessKit's single id
 * space. Defaulted because the first application seen is slot 0, which leaves
 * its ids exactly as they were.
 */
accesskit_node* BuildNode(const IhsSemanticsNode* node,
                          const IhsSemanticsSnapshot* snapshot,
                          uint32_t slot = 0);

/*
 * Node id composition, exposed for the same reason BuildNode is: it is only
 * exercised with an assistive technology attached and several applications
 * running, and every way of getting it wrong is silent. A collision hands a
 * screen reader two different controls under one id; an unstable slot makes an
 * id it is already holding come to mean a different application.
 *
 * AccessKit addresses the whole tree with one 64-bit id while each application
 * numbers its nodes from its own root, so the id carries the application in
 * its high half and the node in its low half.
 */
accesskit_node_id ComposeNodeId(uint32_t slot, int32_t node_id);
uint32_t SlotOfNodeId(accesskit_node_id id);
int32_t NodeOfNodeId(accesskit_node_id id);

/*
 * The slot for a source name: assigned on first sight and never reused, so an
 * id survives other applications starting and stopping. Index in the hub's
 * list would have been the obvious key and is exactly wrong -- it shifts when
 * a source unregisters, silently remapping ids already in use.
 */
uint32_t SlotForSourceName(const std::string& name);

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
  // Rebuilds and pushes the tree spanning every application, if anything has
  // published since the last push.
  void PushSnapshot();

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
