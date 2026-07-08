// shell/logging/dlt/context_cache.hpp
#pragma once

#include "compat.hpp"
#include "libdlt_loader.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

// Note: ihs::flat_map is intentionally not used here. libstdc++14 has a
// non-SFINAE-friendly std::tuple<__convertible_from_tuple_like> path that
// trips on any std::map<std::string, T> insert under Clang-17 / -std=c++23.
// The cache is bounded at kMaxContexts (256) and only searched on the
// (rare) first acquire per call site, so a linear scan over entries_ is
// both portable and fast enough.

namespace ihs::dlt {

enum class ContextError {
  TooManyContexts,
  RegisterFailed,
};

inline constexpr std::size_t kMaxContexts = 256;

struct ContextEntry {
  std::string id;
  std::string description;
  abi::DltContext dlt_ctx{};
};

class ContextCache {
 public:
  explicit ContextCache(LibDltLoader& loader) noexcept : loader_(loader) {}

  [[nodiscard]]
  ihs::expected<std::uint32_t, ContextError> ensure(
      std::string_view ctx_id,
      std::string_view description);

  [[nodiscard]] ContextEntry* at(std::uint32_t index) noexcept;

 private:
  LibDltLoader& loader_;

  // Writers (ensure(), first-use only) serialize on mu_. Readers (at(), the
  // worker's per-slot drain) are lock-free: entries live in fixed storage that
  // never moves and is never removed, so a published index is stable forever.
  // count_ is the release/acquire fence — an entry is fully constructed before
  // count_ is bumped, so a reader that sees index < count_ sees a complete
  // entry.
  std::mutex mu_;
  std::atomic<std::uint32_t> count_{0};
  std::array<std::unique_ptr<ContextEntry>, kMaxContexts> entries_{};
};

}  // namespace ihs::dlt
