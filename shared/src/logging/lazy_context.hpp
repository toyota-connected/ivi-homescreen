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

#ifndef IHS_SHARED_SRC_LOGGING_LAZY_CONTEXT_HPP_
#define IHS_SHARED_SRC_LOGGING_LAZY_CONTEXT_HPP_

#include <atomic>
#include <cstdint>

#include "ihs/logging.h"

namespace ihs::dlt {

// A log context resolved on first use, and re-resolved while it is still
// unresolved.
//
// ihs_log_context_open() answers -1 until ihs_log_start() has run. Caching that
// -1 -- which a plain function-local `static const int32_t` does -- silences
// the call site for the life of the process, so a capability whose first log
// happens to precede logging start never logs again. The shell's
// ihs::log::default_context() hit this and re-attempts while invalid; this is
// that fix for the library's own call sites, which unlike the shell's have no
// single-threaded pre-start window to rely on.
//
// Thread-safe without a lock: two threads racing the open both get the same
// index back (the bridge caches contexts by tag), so a duplicate attempt costs
// a lookup and nothing else. Once resolved the value is stable for the process
// and every later call is one relaxed load.
class LazyLogContext {
 public:
  explicit constexpr LazyLogContext(const char* tag) noexcept : tag_(tag) {}

  LazyLogContext(const LazyLogContext&) = delete;
  LazyLogContext& operator=(const LazyLogContext&) = delete;

  // The context index, or -1 while logging is not up yet. A caller gates on
  // < 0 exactly as it did on the cached value before.
  [[nodiscard]] int32_t index() noexcept {
    const int32_t cached = ctx_.load(std::memory_order_relaxed);
    if (cached >= 0) {
      return cached;
    }
    const int32_t opened = ihs_log_context_open(tag_, nullptr);
    if (opened >= 0) {
      ctx_.store(opened, std::memory_order_relaxed);
    }
    return opened;
  }

 private:
  const char* tag_;
  std::atomic<int32_t> ctx_{-1};
};

}  // namespace ihs::dlt

#endif  // IHS_SHARED_SRC_LOGGING_LAZY_CONTEXT_HPP_
