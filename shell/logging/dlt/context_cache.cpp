// shell/logging/dlt/context_cache.cpp
#include "context_cache.hpp"

#include <cstdio>

namespace ihs::dlt {

ihs::expected<std::uint32_t, ContextError> ContextCache::ensure(
    std::string_view ctx_id,
    std::string_view description) {
  std::lock_guard<std::mutex> lock(mu_);

  // Linear scan: kMaxContexts is small (256) and ensure() is only called
  // on first use of a logging call site, so this stays well off the hot
  // path. See the note in context_cache.hpp for why we don't use std::map.
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i]->id == ctx_id) {
      return static_cast<std::uint32_t>(i);
    }
  }

  if (entries_.size() >= kMaxContexts) {
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

  if (!loader_.register_context(&entry->dlt_ctx, entry->id.c_str(),
                                entry->description.c_str())) {
    return ihs::unexpect<std::uint32_t, ContextError>(
        ContextError::RegisterFailed);
  }

  const auto index = static_cast<std::uint32_t>(entries_.size());
  entries_.push_back(std::move(entry));
  return index;
}

ContextEntry* ContextCache::at(std::uint32_t index) noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  if (index >= entries_.size()) {
    return nullptr;
  }
  return entries_[index].get();
}

}  // namespace ihs::dlt
