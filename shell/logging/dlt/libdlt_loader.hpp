// shell/logging/dlt/libdlt_loader.hpp
// Runtime dlopen loader for libdlt.so.2. Self-contained ABI types live in
// ihs::dlt::abi so the new bridge does not collide with legacy libdlt.h.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ihs::dlt::abi {

inline constexpr int kDltIdSize = 4;

// Mirrors genivi/dlt-daemon DltContext layout.
struct DltContext {
    char         id[kDltIdSize];
    std::int32_t pos;
    std::int8_t* p1;
    std::int8_t* p2;
    std::uint8_t count;
};

// DltContextData is variable across DLT versions; we only ever interact with
// it through libdlt's own entry points, so we allocate an opaque scratch
// buffer sized generously to cover the observed layout.
inline constexpr std::size_t kContextDataSize = 512;

struct alignas(16) OpaqueContextData {
    unsigned char bytes[kContextDataSize];
};

} // namespace ihs::dlt::abi

namespace ihs::dlt {

class LibDltLoader {
public:
    static LibDltLoader& instance();

    [[nodiscard]] bool available() const noexcept { return available_; }

    // One-shot: register an application id with libdlt (no-op if unavailable).
    bool register_app(const char* app_id, const char* description) noexcept;
    void unregister_app() noexcept;

    // Context registration. Returns true on success or when libdlt is absent
    // (the caller still gets a valid handle for the spdlog fallback path).
    bool register_context(abi::DltContext* ctx,
                          const char*      ctx_id,
                          const char*      description) noexcept;
    void unregister_context(abi::DltContext* ctx) noexcept;

    // Emits a single log line. Silently drops if libdlt is unavailable.
    void emit(abi::DltContext* ctx, int level, const char* text) noexcept;

private:
    LibDltLoader();
    ~LibDltLoader();
    LibDltLoader(const LibDltLoader&)            = delete;
    LibDltLoader& operator=(const LibDltLoader&) = delete;

    void load();

    void* handle_    = nullptr;
    bool  available_ = false;

    // Function pointer table — using int return type to match DltReturnValue.
    int (*fn_register_app_)(const char*, const char*)                       = nullptr;
    int (*fn_unregister_app_)()                                             = nullptr;
    int (*fn_register_context_)(abi::DltContext*, const char*, const char*) = nullptr;
    int (*fn_unregister_context_)(abi::DltContext*)                         = nullptr;
    int (*fn_log_write_start_)(abi::DltContext*, void*, int)                = nullptr;
    int (*fn_log_write_finish_)(void*)                                      = nullptr;
    int (*fn_log_write_string_)(void*, const char*)                         = nullptr;
};

} // namespace ihs::dlt
