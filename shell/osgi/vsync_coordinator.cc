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

#include "vsync_coordinator.h"

#include <algorithm>
#include <utility>

#include "logging/logging.h"
#include "vsync/ivsync_provider.h"

namespace ihs::osgi {

ProviderVsyncTarget::ProviderVsyncTarget(ivi::IVsyncProvider* provider)
    : provider_(provider) {}

void ProviderVsyncTarget::OnVsync(const uint64_t frame_start_ns) {
  if (provider_ != nullptr) {
    // Non-blocking: the provider marshals OnVsync onto its own engine's
    // platform runner, so a stalled engine cannot delay the targets behind it.
    provider_->DeliverVsync(frame_start_ns);
  }
}

bool VsyncCoordinator::Add(const std::string& symbolic_name,
                           const Priority priority,
                           std::shared_ptr<IVsyncTarget> target) {
  if (symbolic_name.empty()) {
    ihs::log::error("[osgi] vsync: refusing to register an unnamed target");
    return false;
  }
  if (target == nullptr) {
    ihs::log::error("[osgi] vsync: bundle '{}' supplied a null vsync target",
                    symbolic_name);
    return false;
  }

  std::lock_guard lock(mutex_);
  const auto existing = std::find_if(entries_.begin(), entries_.end(),
                                     [&symbolic_name](const Entry& e) {
                                       return e.symbolic_name == symbolic_name;
                                     });
  if (existing != entries_.end()) {
    // Rebinding silently would leave the previous engine's baton unanswered,
    // stalling a pipeline that looks healthy from the outside.
    ihs::log::error("[osgi] vsync: bundle '{}' is already registered",
                    symbolic_name);
    return false;
  }

  const Entry entry{symbolic_name, priority, next_sequence_++,
                    std::move(target)};
  // Insert in dispatch order rather than sorting per tick: registration is
  // rare, ticks are 60Hz.
  const auto at = std::upper_bound(entries_.begin(), entries_.end(), entry,
                                   [](const Entry& a, const Entry& b) {
                                     if (a.priority != b.priority) {
                                       return a.priority < b.priority;
                                     }
                                     return a.sequence < b.sequence;
                                   });
  entries_.insert(at, entry);
  return true;
}

bool VsyncCoordinator::Remove(const std::string& symbolic_name) {
  std::lock_guard lock(mutex_);
  const auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&symbolic_name](const Entry& e) {
                                 return e.symbolic_name == symbolic_name;
                               });
  if (it == entries_.end()) {
    return false;
  }
  entries_.erase(it);
  return true;
}

size_t VsyncCoordinator::Tick(const uint64_t frame_start_ns) {
  // Snapshot under the lock, dispatch outside it. Copying the shared_ptrs is
  // what keeps a target removed mid-tick alive for the delivery already in
  // flight; dropping the lock first is what stops a target that calls back into
  // Add/Remove from deadlocking, and keeps one slow target off the critical
  // path of the rest.
  std::vector<std::shared_ptr<IVsyncTarget>> snapshot;
  {
    std::lock_guard lock(mutex_);
    snapshot.reserve(entries_.size());
    for (const auto& entry : entries_) {
      snapshot.push_back(entry.target);
    }
  }

  for (const auto& target : snapshot) {
    target->OnVsync(frame_start_ns);
  }
  return snapshot.size();
}

size_t VsyncCoordinator::size() const {
  std::lock_guard lock(mutex_);
  return entries_.size();
}

bool VsyncCoordinator::Contains(const std::string& symbolic_name) const {
  std::lock_guard lock(mutex_);
  return std::any_of(entries_.begin(), entries_.end(),
                     [&symbolic_name](const Entry& e) {
                       return e.symbolic_name == symbolic_name;
                     });
}

std::vector<std::string> VsyncCoordinator::DispatchOrder() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> names;
  names.reserve(entries_.size());
  for (const auto& entry : entries_) {
    names.push_back(entry.symbolic_name);
  }
  return names;
}

}  // namespace ihs::osgi
