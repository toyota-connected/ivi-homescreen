// shell/logging/dlt/context_cache.cpp
#include "context_cache.hpp"

#include <cstdio>

namespace ihs::dlt {

ihs::expected<std::uint32_t, ContextError>
ContextCache::ensure(std::string_view ctx_id, std::string_view description) {
    std::lock_guard<std::mutex> lock(mu_);

    const std::string key(ctx_id);
    auto it = id_to_handle_.find(key);
    if (it != id_to_handle_.end()) {
        return it->second;
    }

    if (entries_.size() >= kMaxContexts) {
        // Error path: bypass ihs::println because its format placeholder
        // syntax ({} vs %) diverges between the C++20+ and C++17 backends.
        std::fprintf(stderr,
                     "[dlt] ContextCache: max %zu contexts reached\n",
                     kMaxContexts);
        return ihs::unexpect<std::uint32_t, ContextError>(
                   ContextError::TooManyContexts);
    }

    auto entry         = std::make_unique<ContextEntry>();
    entry->id          = key;
    entry->description = std::string(description);

    if (!loader_.register_context(&entry->dlt_ctx,
                                  entry->id.c_str(),
                                  entry->description.c_str())) {
        return ihs::unexpect<std::uint32_t, ContextError>(
                   ContextError::RegisterFailed);
    }

    const auto index = static_cast<std::uint32_t>(entries_.size());
    entries_.push_back(std::move(entry));
    id_to_handle_[key] = index;
    return index;
}

ContextEntry* ContextCache::at(std::uint32_t index) noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    if (index >= entries_.size()) {
        return nullptr;
    }
    return entries_[index].get();
}

} // namespace ihs::dlt
