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

#include "bundle_state.h"

#include <utility>

#include "logging/logging.h"

namespace ihs::osgi {

std::string_view BundleStateName(const BundleState state) {
  switch (state) {
    case BundleState::kUninstalled:
      return "UNINSTALLED";
    case BundleState::kInstalled:
      return "INSTALLED";
    case BundleState::kResolved:
      return "RESOLVED";
    case BundleState::kStarting:
      return "STARTING";
    case BundleState::kStopping:
      return "STOPPING";
    case BundleState::kActive:
      return "ACTIVE";
  }
  return "UNKNOWN";
}

bool IsLegalTransition(const BundleState from, const BundleState to) {
  if (from == to) {
    // Idempotent re-entry is not an edge. Callers that poll a state should test
    // it, not re-assert it, so treating this as legal would hide a caller that
    // has lost track of where the bundle is.
    return false;
  }
  switch (from) {
    case BundleState::kInstalled:
      return to == BundleState::kResolved || to == BundleState::kUninstalled;
    case BundleState::kResolved:
      // Back to INSTALLED covers an unresolve (a dependency went away).
      return to == BundleState::kStarting || to == BundleState::kInstalled ||
             to == BundleState::kUninstalled;
    case BundleState::kStarting:
      // STOPPING is the failure exit: an activator that throws, or a critical
      // bundle that misses its deadline, unwinds the same way a healthy stop
      // does.
      return to == BundleState::kActive || to == BundleState::kStopping;
    case BundleState::kActive:
      return to == BundleState::kStopping;
    case BundleState::kStopping:
      // Back to RESOLVED, not on to UNINSTALLED: this is what makes restart a
      // normal operation rather than a teardown.
      return to == BundleState::kResolved;
    case BundleState::kUninstalled:
      return false;
  }
  return false;
}

BundleStateMachine::BundleStateMachine(std::string symbolic_name)
    : symbolic_name_(std::move(symbolic_name)) {}

bool BundleStateMachine::Transition(const BundleState to) {
  if (!IsLegalTransition(state_, to)) {
    ihs::log::error("[osgi] bundle '{}': illegal transition {} -> {}",
                    symbolic_name_, BundleStateName(state_),
                    BundleStateName(to));
    return false;
  }
  ihs::log::debug("[osgi] bundle '{}': {} -> {}", symbolic_name_,
                  BundleStateName(state_), BundleStateName(to));
  state_ = to;
  return true;
}

}  // namespace ihs::osgi
