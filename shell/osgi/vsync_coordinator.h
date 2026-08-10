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
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "priority.h"

namespace ivi {
class IVsyncProvider;
}

namespace ihs::osgi {

// Something that can be handed a vsync tick.
//
// IVsyncProvider::DeliverVsync is not virtual, and a live provider needs an
// engine and a platform task runner, so the coordinator talks to this instead.
// Production wraps a provider with ProviderVsyncTarget; tests substitute a
// recorder. The ordering and lifetime rules -- which is what this class exists
// to get right -- are then testable without an engine.
class IVsyncTarget {
 public:
  virtual ~IVsyncTarget() = default;

  // Hand this target the tick. Must not block: it runs on the source event
  // thread, ahead of every later target in the dispatch order.
  virtual void OnVsync(uint64_t frame_start_ns) = 0;
};

// Adapts an ivi::IVsyncProvider to IVsyncTarget. Does not own the provider,
// which outlives its engine and is owned by the backend.
class ProviderVsyncTarget final : public IVsyncTarget {
 public:
  explicit ProviderVsyncTarget(ivi::IVsyncProvider* provider);
  void OnVsync(uint64_t frame_start_ns) override;

 private:
  ivi::IVsyncProvider* provider_;
};

// Fans one presentation source out to every bundle engine, in priority order.
//
// Each Flutter engine needs its own vsync baton, and ivi::IVsyncProvider is
// consequently 1:1 with an engine -- its engine handle, parked baton and task
// runner are all single-valued. Running N engines therefore means N providers,
// and the thing that does not scale is the *source*: one display has one
// vblank, and letting each engine drive its own timer against it produces
// tearing and drift. This class is the missing multiplexer -- one source event
// in, N ordered deliveries out.
//
// Order is (priority, registration sequence). Critical bundles are served
// first on every tick, so an instrument cluster gets its baton before an
// infotainment view competes for the same vblank; ties break by registration
// so the order is stable and the startup log reproducible.
//
// Threading: Tick() runs on the source event thread (the Wayland event loop or
// the DRM fd handler) while Add/Remove run on the platform thread or a bundle
// startup thread. Delivery happens against a snapshot taken under the lock and
// released before dispatch, for two reasons: a target must never be able to
// deadlock the coordinator by calling back into it, and holding a lock across
// N deliveries would put the slowest target on the critical path of every
// later one. Snapshot entries are shared_ptr, so a target removed mid-tick
// stays alive until the tick that already picked it up completes.
class VsyncCoordinator {
 public:
  VsyncCoordinator() = default;

  VsyncCoordinator(const VsyncCoordinator&) = delete;
  VsyncCoordinator& operator=(const VsyncCoordinator&) = delete;

  // Register @target under @symbolic_name. Returns false on a duplicate name,
  // an empty name, or a null target.
  bool Add(const std::string& symbolic_name,
           Priority priority,
           std::shared_ptr<IVsyncTarget> target);

  // Deregister. Returns true if the name was known. A tick already dispatching
  // may still deliver to it once; the snapshot holds it alive until then.
  bool Remove(const std::string& symbolic_name);

  // Deliver @frame_start_ns to every registered target in dispatch order.
  // Returns the number delivered. Safe to call concurrently with Add/Remove.
  size_t Tick(uint64_t frame_start_ns);

  [[nodiscard]] size_t size() const;

  [[nodiscard]] bool Contains(const std::string& symbolic_name) const;

  // Names in dispatch order. For tests and the startup log.
  [[nodiscard]] std::vector<std::string> DispatchOrder() const;

 private:
  struct Entry {
    std::string symbolic_name;
    Priority priority{Priority::kNormal};
    // Monotonic, so ties within a priority keep registration order.
    uint64_t sequence{0};
    std::shared_ptr<IVsyncTarget> target;
  };

  mutable std::mutex mutex_;
  // Kept sorted by (priority, sequence) so Tick does no work beyond copying.
  std::vector<Entry> entries_;
  uint64_t next_sequence_{0};
};

}  // namespace ihs::osgi
