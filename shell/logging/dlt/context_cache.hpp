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

namespace ihs::dlt {

enum class ContextError {
    TooManyContexts,
    RegisterFailed,
};

inline constexpr std::size_t kMaxContexts = 256;

struct ContextEntry {
    std::string     id;
    std::string     description;
    abi::DltContext dlt_ctx{};
};

class ContextCache {
public:
    explicit ContextCache(LibDltLoader& loader) noexcept : loader_(loader) {}

    [[nodiscard]]
    ihs::expected<std::uint32_t, ContextError>
    ensure(std::string_view ctx_id, std::string_view description);

    [[nodiscard]] ContextEntry* at(std::uint32_t index) noexcept;

private:
    LibDltLoader&                                loader_;
    std::mutex                                   mu_;
    ihs::flat_map<std::string, std::uint32_t>    id_to_handle_;
    std::vector<std::unique_ptr<ContextEntry>>   entries_;
};

} // namespace ihs::dlt
