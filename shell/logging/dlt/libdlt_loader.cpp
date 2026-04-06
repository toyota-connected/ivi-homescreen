// shell/logging/dlt/libdlt_loader.cpp
#include "libdlt_loader.hpp"

#include <dlfcn.h>

#include <cstring>

namespace ihs::dlt {

namespace {

template <typename Fn>
void resolve(void* handle, const char* name, Fn& out) noexcept {
    out = reinterpret_cast<Fn>(::dlsym(handle, name));
}

} // namespace

LibDltLoader& LibDltLoader::instance() {
    static LibDltLoader the_loader;
    return the_loader;
}

LibDltLoader::LibDltLoader() {
    load();
}

LibDltLoader::~LibDltLoader() {
    if (handle_ != nullptr && handle_ != RTLD_DEFAULT) {
        ::dlclose(handle_);
    }
}

void LibDltLoader::load() {
    // Prefer an already-loaded symbol from the global scope (mirrors the
    // behavior of the legacy LibDlt loader).
    if (::dlsym(RTLD_DEFAULT, "dlt_user_log_write_start") != nullptr) {
        handle_ = RTLD_DEFAULT;
    } else {
        handle_ = ::dlopen("libdlt.so.2", RTLD_LAZY | RTLD_LOCAL);
    }
    if (handle_ == nullptr) {
        return;
    }

    resolve(handle_, "dlt_register_app",          fn_register_app_);
    resolve(handle_, "dlt_unregister_app",        fn_unregister_app_);
    resolve(handle_, "dlt_register_context",      fn_register_context_);
    resolve(handle_, "dlt_unregister_context",    fn_unregister_context_);
    resolve(handle_, "dlt_user_log_write_start",  fn_log_write_start_);
    resolve(handle_, "dlt_user_log_write_finish", fn_log_write_finish_);
    resolve(handle_, "dlt_user_log_write_string", fn_log_write_string_);

    available_ = (fn_log_write_start_  != nullptr &&
                  fn_log_write_finish_ != nullptr &&
                  fn_log_write_string_ != nullptr);
}

bool LibDltLoader::register_app(const char* app_id,
                                const char* description) noexcept {
    if (!available_ || fn_register_app_ == nullptr) {
        return false;
    }
    return fn_register_app_(app_id, description) == 0;
}

void LibDltLoader::unregister_app() noexcept {
    if (available_ && fn_unregister_app_ != nullptr) {
        fn_unregister_app_();
    }
}

bool LibDltLoader::register_context(abi::DltContext* ctx,
                                    const char*      ctx_id,
                                    const char*      description) noexcept {
    if (ctx == nullptr) {
        return false;
    }
    std::memset(ctx, 0, sizeof(*ctx));
    if (ctx_id != nullptr) {
        std::size_t n = std::strlen(ctx_id);
        if (n > abi::kDltIdSize) n = abi::kDltIdSize;
        std::memcpy(ctx->id, ctx_id, n);
    }
    if (!available_ || fn_register_context_ == nullptr) {
        // Still return "ok" — the bridge keeps the handle for fallback paths.
        return true;
    }
    return fn_register_context_(ctx, ctx_id, description) == 0;
}

void LibDltLoader::unregister_context(abi::DltContext* ctx) noexcept {
    if (available_ && fn_unregister_context_ != nullptr && ctx != nullptr) {
        fn_unregister_context_(ctx);
    }
}

void LibDltLoader::emit(abi::DltContext* ctx,
                        int              level,
                        const char*      text) noexcept {
    if (!available_ || ctx == nullptr || text == nullptr) {
        return;
    }
    abi::OpaqueContextData scratch{};
    if (fn_log_write_start_(ctx, &scratch, level) < 0) {
        return;
    }
    fn_log_write_string_(&scratch, text);
    fn_log_write_finish_(&scratch);
}

} // namespace ihs::dlt
