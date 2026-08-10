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

#include <cstdint>
#include <string>
#include <string_view>

namespace ihs::osgi {

// The OSGi bundle lifecycle states.
//
// Values follow the OSGi core specification's Bundle constants so a Dart-side
// BundleState can be sent across as an int without a translation table.
enum class BundleState : uint32_t {
  kUninstalled = 0x01,
  kInstalled = 0x02,
  kResolved = 0x04,
  kStarting = 0x08,
  kStopping = 0x10,
  kActive = 0x20,
};

std::string_view BundleStateName(BundleState state);

// Whether @from -> @to is a legal lifecycle edge.
//
// The graph is the OSGi one, not the simplified linear chain:
//
//   INSTALLED   -> RESOLVED | UNINSTALLED
//   RESOLVED    -> STARTING | INSTALLED | UNINSTALLED
//   STARTING    -> ACTIVE | STOPPING
//   ACTIVE      -> STOPPING
//   STOPPING    -> RESOLVED
//   UNINSTALLED -> (terminal)
//
// Two edges matter more than the rest here. STOPPING returns to RESOLVED
// rather than running on to UNINSTALLED, which is what makes restart a normal
// operation instead of a teardown -- the restart-with-backoff path needs to
// come back round to STARTING. And STARTING -> STOPPING exists so an activator
// that throws, or a critical bundle that misses its startup deadline, unwinds
// through the same path as a healthy stop rather than needing a special case.
[[nodiscard]] bool IsLegalTransition(BundleState from, BundleState to);

// One bundle's lifecycle position, with illegal edges rejected rather than
// applied.
//
// Not thread-safe by design: each instance is owned by the orchestrator and
// mutated from the thread driving that bundle's startup. Sharing one across
// threads would need external synchronization.
class BundleStateMachine {
 public:
  // A bundle exists as soon as its manifest is parsed, which is INSTALLED.
  explicit BundleStateMachine(std::string symbolic_name);

  [[nodiscard]] BundleState state() const { return state_; }
  [[nodiscard]] const std::string& symbolic_name() const {
    return symbolic_name_;
  }

  // Apply a transition. Returns false and logs, leaving the state untouched,
  // when the edge is not legal -- a rejected transition is a bug in the caller,
  // and silently applying it would put a bundle in a state its owner does not
  // expect.
  bool Transition(BundleState to);

  // True once the bundle can no longer be started again.
  [[nodiscard]] bool IsTerminal() const {
    return state_ == BundleState::kUninstalled;
  }

  // True while the bundle is running or on its way to running. The bridge and
  // the vsync coordinator should hold a registration exactly across this.
  [[nodiscard]] bool IsLive() const {
    return state_ == BundleState::kStarting || state_ == BundleState::kActive;
  }

 private:
  std::string symbolic_name_;
  BundleState state_{BundleState::kInstalled};
};

}  // namespace ihs::osgi
