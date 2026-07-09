// shell/logging/dlt/context_cache.cpp
#include "context_cache.hpp"

#include <cstdio>

namespace ihs::dlt {

ihs::expected<std::uint32_t, ContextError> ContextCache::ensure(
    std::string_view ctx_id,
    std::string_view description) {
  std::lock_guard<std::mutex> lock(mu_);

  const std::uint32_t count = count_.load(std::memory_order_relaxed);

  // Linear scan: kMaxContexts is small (256) and ensure() is only called
  // on first use of a logging call site, so this stays well off the hot
  // path. See the note in context_cache.hpp for why we don't use std::map.
  for (std::uint32_t i = 0; i < count; ++i) {
    if (entries_[i]->id == ctx_id) {
      return i;
    }
  }

  if (count >= kMaxContexts) {
    // Error path: bypass ihs::println because its format placeholder
    // syntax ({} vs %) diverges between the C++20+ and C++17 backends.
    std::fprintf(stderr, "[dlt] ContextCache: max %zu contexts reached\n",
                 kMaxContexts);
    return ihs::unexpect<std::uint32_t, ContextError>(
        ContextError::TooManyContexts);
  }

  auto entry = std::make_unique<ContextEntry>();
  entry->id = std::string(ctx_id);
  entry->description = std::string(description);

#if ENABLE_DLT
  // Best-effort DLT registration: a failure (libdlt absent) leaves dlt_ctx
  // unregistered, but the entry is still stored so its tag serves the console
  // and file sinks. Only the DLT sink needs a registered context; a build
  // without DLT skips registration entirely and leaves dlt_ctx zeroed.
  LibDltLoader::instance().register_context(&entry->dlt_ctx, entry->id.c_str(),
                                            entry->description.c_str());
#endif

  entries_[count] = std::move(entry);
  // Release: the entry is fully constructed above; publish the new count so a
  // lock-free at() that observes it (acquire) also observes the entry.
  count_.store(count + 1, std::memory_order_release);
  return count;
}

ContextEntry* ContextCache::at(std::uint32_t index) noexcept {
  // Lock-free: fixed storage, never moved or removed. Acquire pairs with the
  // release store in ensure().
  if (index >= count_.load(std::memory_order_acquire)) {
    return nullptr;
  }
  return entries_[index].get();
}

}  // namespace ihs::dlt
