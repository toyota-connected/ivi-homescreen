// shell/logging/dlt/libdlt_loader.cpp
#include "libdlt_loader.hpp"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ihs::dlt {

namespace {

template <typename Fn>
void resolve(void* handle, const char* name, Fn& out) noexcept {
  out = reinterpret_cast<Fn>(::dlsym(handle, name));
}

const char* reason_to_str(DltDisableReason r) noexcept {
  switch (r) {
    case DltDisableReason::Ok:
      return "ok";
    case DltDisableReason::LibraryNotFound:
      return "library-not-found";
    case DltDisableReason::VersionCheckMissing:
      return "version-check-missing";
    case DltDisableReason::VersionMismatch:
      return "version-mismatch";
    case DltDisableReason::RequiredSymbolMissing:
      return "required-symbol-missing";
    case DltDisableReason::ScratchOverrun:
      return "scratch-overrun";
  }
  return "unknown";
}

}  // namespace

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

void LibDltLoader::disable(DltDisableReason reason,
                           const char* detail) noexcept {
  available_.store(false, std::memory_order_release);
  disabled_reason_ = reason;
  // One-shot stderr line so the operator knows why DLT went silent.
  std::fprintf(stderr, "[dlt] bridge disabled: %s%s%s\n", reason_to_str(reason),
               detail ? " — " : "", detail ? detail : "");
}

void LibDltLoader::load() {
  // Layer 4.2: env override. IHS_DLT_LIBRARY=/path/to/libdlt.so.2 lets
  // ops point at a known-good build during incident response without
  // recompiling. Falls back to the standard soname.
  const char* override_path = std::getenv("IHS_DLT_LIBRARY");
  const char* soname = (override_path != nullptr && override_path[0] != '\0')
                           ? override_path
                           : "libdlt.so.2";

  // Prefer an already-loaded symbol from the global scope (mirrors the
  // legacy LibDlt loader). RTLD_DEFAULT bypasses our soname/override but
  // is what the host process actually has linked, so it wins when present.
  if (::dlsym(RTLD_DEFAULT, "dlt_user_log_write_start") != nullptr) {
    handle_ = RTLD_DEFAULT;
  } else {
    // Layer 4.1: RTLD_NOW forces all symbol resolutions at load time so
    // a libdlt with missing entry points fails dlopen() rather than
    // blowing up later inside the worker thread.
    handle_ = ::dlopen(soname, RTLD_NOW | RTLD_LOCAL);
  }

  if (handle_ == nullptr) {
    disable(DltDisableReason::LibraryNotFound, ::dlerror());
    return;
  }

  // Layer 1: resolve and run dlt_check_library_version *first*, before
  // touching any other API.
  resolve(handle_, "dlt_check_library_version", fn_check_version_);
  resolve(handle_, "dlt_get_version", fn_get_version_);

  if (fn_check_version_ == nullptr) {
    // Pre-2.13 or stripped — refuse rather than risk a layout mismatch.
    disable(DltDisableReason::VersionCheckMissing,
            "dlt_check_library_version not exported");
    return;
  }

  if (fn_check_version_(abi::kDltExpectedMajor, abi::kDltExpectedMinor) != 0) {
    char detail[256];
    if (fn_get_version_ != nullptr) {
      char buf[192] = {};
      fn_get_version_(buf, sizeof(buf));
      std::snprintf(detail, sizeof(detail), "expected %s.%s, loaded: %s",
                    abi::kDltExpectedMajor, abi::kDltExpectedMinor, buf);
    } else {
      std::snprintf(detail, sizeof(detail), "expected %s.%s",
                    abi::kDltExpectedMajor, abi::kDltExpectedMinor);
    }
    disable(DltDisableReason::VersionMismatch, detail);
    return;
  }

  // Optional: log what was loaded so operators can correlate disable
  // events later.
  if (fn_get_version_ != nullptr) {
    char buf[192] = {};
    fn_get_version_(buf, sizeof(buf));
    std::fprintf(stderr, "[dlt] loaded: %s\n", buf);
  }

  // Layer 1 (continued): resolve the rest of the required surface and
  // refuse if anything is missing.
  resolve(handle_, "dlt_register_app", fn_register_app_);
  resolve(handle_, "dlt_unregister_app", fn_unregister_app_);
  resolve(handle_, "dlt_register_context", fn_register_context_);
  resolve(handle_, "dlt_unregister_context", fn_unregister_context_);
  resolve(handle_, "dlt_user_log_write_start", fn_log_write_start_);
  resolve(handle_, "dlt_user_log_write_finish", fn_log_write_finish_);
  resolve(handle_, "dlt_user_log_write_string", fn_log_write_string_);

  // Layer 3: optional single-shot fast path. Probed but not required.
  // When present, emit() uses it and never touches the scratch buffer.
  resolve(handle_, "dlt_log_string", fn_log_string_oneshot_);

  const bool required_ok =
      fn_register_app_ != nullptr && fn_register_context_ != nullptr &&
      fn_log_write_start_ != nullptr && fn_log_write_finish_ != nullptr &&
      fn_log_write_string_ != nullptr;

  if (!required_ok) {
    disable(DltDisableReason::RequiredSymbolMissing,
            "one or more dlt_user_log_write_* entry points are null");
    return;
  }

  available_.store(true, std::memory_order_release);
  disabled_reason_ = DltDisableReason::Ok;
}

bool LibDltLoader::register_app(const char* app_id,
                                const char* description) noexcept {
  if (!available() || fn_register_app_ == nullptr) {
    return false;
  }
  return fn_register_app_(app_id, description) == 0;
}

void LibDltLoader::unregister_app() noexcept {
  if (available() && fn_unregister_app_ != nullptr) {
    fn_unregister_app_();
  }
}

bool LibDltLoader::register_context(abi::DltContext* ctx,
                                    const char* ctx_id,
                                    const char* description) noexcept {
  if (ctx == nullptr) {
    return false;
  }
  std::memset(ctx, 0, sizeof(*ctx));
  if (ctx_id != nullptr) {
    std::size_t n = std::strlen(ctx_id);
    if (n > abi::kDltIdSize)
      n = abi::kDltIdSize;
    std::memcpy(ctx->id, ctx_id, n);
  }
  if (!available() || fn_register_context_ == nullptr) {
    // Still return "ok" — the bridge keeps the handle for fallback paths.
    return true;
  }
  return fn_register_context_(ctx, ctx_id, description) == 0;
}

void LibDltLoader::unregister_context(abi::DltContext* ctx) noexcept {
  if (available() && fn_unregister_context_ != nullptr && ctx != nullptr) {
    fn_unregister_context_(ctx);
  }
}

void LibDltLoader::emit(abi::DltContext* ctx,
                        int level,
                        const char* text) noexcept {
  if (!available()) {
    // Layer 5: count drops attributable to a self-disabled bridge.
    degraded_drops_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (ctx == nullptr || text == nullptr) {
    return;
  }

  // Layer 3: single-shot fast path. When present this avoids the
  // GuardedContextData scratch entirely, eliminating the layout-mismatch
  // class of bug for this code path.
  if (fn_log_string_oneshot_ != nullptr) {
    fn_log_string_oneshot_(ctx, level, text);
    return;
  }

  // Layer 2: per-thread heap-backed (TLS) scratch with sentinel guards.
  // The scratch is *not* on the worker's stack frame, so a libdlt overrun
  // smashes a thread-local heap region (debuggable, ASan-visible) rather
  // than the worker's return address. The guards catch overruns *the
  // first time they happen* and self-disable the bridge.
  //
  // The scratch is zero-initialized exactly once per thread (TLS init);
  // dlt_user_log_write_start is responsible for (re)initializing the
  // DltContextData each call, so we deliberately do not memset the bytes
  // on every emit. The guards bracket the buffer so an overrun is still
  // caught — we just trust libdlt to own the payload contents between
  // start and finish.
  thread_local abi::GuardedContextData scratch{};

  if (fn_log_write_start_(ctx, scratch.bytes, level) < 0) {
    return;
  }
  fn_log_write_string_(scratch.bytes, text);
  fn_log_write_finish_(scratch.bytes);

  if (scratch.head_guard != abi::kScratchGuardPattern ||
      scratch.tail_guard != abi::kScratchGuardPattern) {
    // Sentinel violation — refuse further emits and surface why. Use
    // the disable() helper so the reason is recorded uniformly.
    char detail[160];
    std::snprintf(detail, sizeof(detail),
                  "DltContextData overran %zu B scratch "
                  "(head=0x%016llx tail=0x%016llx)",
                  abi::kContextDataSize,
                  static_cast<unsigned long long>(scratch.head_guard),
                  static_cast<unsigned long long>(scratch.tail_guard));
    disable(DltDisableReason::ScratchOverrun, detail);
  }
}

}  // namespace ihs::dlt
