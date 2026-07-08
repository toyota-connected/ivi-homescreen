// shell/logging/dlt/context_cache.hpp
#pragma once

#include "compat.hpp"
#include "libdlt_loader.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

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
  std::mutex mu_;
  std::vector<std::unique_ptr<ContextEntry>> entries_;
};

}  // namespace ihs::dlt
