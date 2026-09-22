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
#include <set>
#include <vector>

// Buffer ids a producer retired (ihs_pv_retire_buffer) that could not be
// dropped at once because they were on screen: the view's current frame, or
// the frame waiting to become current. Such an id is released once it is off
// screen, which the caller checks whenever the frames on screen change.
//
// Holds ids only; the caller owns the imports and decides what "on screen"
// means for its path. Not thread-safe: the caller's view lock guards it.
class DeferredRetireSet {
 public:
  // The producer retired `id`. Returns true when the caller may drop the
  // import now; false when `on_screen`, in which case the id is remembered
  // until ReleaseOffScreen() hands it back.
  bool Retire(uint32_t id, bool on_screen) {
    if (on_screen) {
      ids_.insert(id);
      return false;
    }
    ids_.erase(id);
    return true;
  }

  // A submit of `id` arrived. Returns true when `id` had been retired while on
  // screen: the producer has since re-used the id, possibly for a different
  // dma-buf, so the caller must re-import rather than trust the cached import.
  bool Resubmitted(uint32_t id) { return ids_.erase(id) != 0; }

  // The frames on screen changed. Returns the retired ids for which
  // `on_screen(id)` is now false, which the caller may drop; the rest stay
  // deferred.
  template <typename OnScreen>
  std::vector<uint32_t> ReleaseOffScreen(const OnScreen& on_screen) {
    std::vector<uint32_t> released;
    for (auto it = ids_.begin(); it != ids_.end();) {
      if (!on_screen(*it)) {
        released.push_back(*it);
        it = ids_.erase(it);
      } else {
        ++it;
      }
    }
    return released;
  }

  [[nodiscard]] bool Contains(uint32_t id) const { return ids_.count(id) != 0; }
  [[nodiscard]] size_t size() const { return ids_.size(); }
  void Clear() { ids_.clear(); }

 private:
  std::set<uint32_t> ids_;
};
