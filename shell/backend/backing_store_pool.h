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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

/**
 * @brief Reusable pool of backing stores keyed by (width, height).
 *
 * The Flutter engine reuses backing stores across frames when sizes match; this
 * pool mirrors that with a free-list. It is intentionally backend-agnostic —
 * any type that provides @c Width() and @c Height() and is safely destructible
 * is acceptable.
 *
 * Thread safety: @c Acquire, @c Release, and @c Flush are callable from any
 * thread. Callbacks arrive on the rasterizer thread.
 */
template <typename BackingStore>
class BackingStorePool {
 public:
  BackingStorePool() = default;
  ~BackingStorePool() { Flush(); }

  BackingStorePool(const BackingStorePool&) = delete;
  BackingStorePool& operator=(const BackingStorePool&) = delete;

  /**
   * @brief Return a store from the free-list, or construct a new one.
   *
   * @param width  Desired backing store width in pixels.
   * @param height Desired backing store height in pixels.
   * @param args   Forwarded to @c BackingStore constructor when allocating.
   * @return shared_ptr to a backing store of the requested dimensions.
   */
  template <typename... Args>
  std::shared_ptr<BackingStore> Acquire(int32_t width,
                                        int32_t height,
                                        Args&&... args) {
    std::lock_guard<std::mutex> lock(mu_);
    const Key key{width, height};
    const auto it = free_list_.find(key);
    if (it != free_list_.end()) {
      auto store = std::move(it->second);
      free_list_.erase(it);
      return store;
    }
    return std::make_shared<BackingStore>(width, height,
                                          std::forward<Args>(args)...);
  }

  /**
   * @brief Return a store to the free-list for reuse.
   */
  void Release(std::shared_ptr<BackingStore> store) {
    if (!store) {
      return;
    }
    const int32_t w = store->Width();
    const int32_t h = store->Height();
    std::lock_guard<std::mutex> lock(mu_);
    if (capacity_ > 0 && free_list_.size() >= capacity_) {
      return;
    }
    free_list_.emplace(Key{w, h}, std::move(store));
  }

  /**
   * @brief Destroy all pooled stores. Call before context teardown.
   */
  void Flush() {
    std::lock_guard<std::mutex> lock(mu_);
    free_list_.clear();
  }

  /**
   * @brief Cap the pool at @p n free entries. 0 means unbounded (default).
   *
   * When the cap is hit, further @c Release calls drop the store instead of
   * pooling it. Useful to bound memory under pathological layout churn.
   */
  void SetCapacity(size_t n) {
    std::lock_guard<std::mutex> lock(mu_);
    capacity_ = n;
    while (capacity_ > 0 && free_list_.size() > capacity_) {
      free_list_.erase(free_list_.begin());
    }
  }

  /**
   * @brief Current number of pooled (free) stores.
   */
  size_t Size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return free_list_.size();
  }

 private:
  using Key = std::pair<int32_t, int32_t>;

  struct PairHash {
    size_t operator()(const Key& k) const noexcept {
      return std::hash<int64_t>{}(
          (static_cast<int64_t>(k.first) << 32) ^
          static_cast<int64_t>(static_cast<uint32_t>(k.second)));
    }
  };

  mutable std::mutex mu_;
  std::unordered_multimap<Key, std::shared_ptr<BackingStore>, PairHash>
      free_list_;
  size_t capacity_{0};
};
