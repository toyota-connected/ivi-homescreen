// shell/logging/dlt/libdlt_loader.hpp
//
// Runtime dlopen loader for libdlt.so.2.
//
// Compatibility / safety strategy:
//
//   Layer 1: dlt_check_library_version is resolved and called *before* the
//            loader exposes any other entry point. Refusal disables the
//            bridge cleanly (available_ = false).
//
//   Layer 2: DltContextData scratch is per-thread, heap-backed (TLS), sized
//            generously (kContextDataSize), and bracketed by sentinel
//            guards that are checked after every emit(). A guard violation
//            self-disables the bridge.
//
//   Layer 3: emit() prefers a single-shot string entry point (dlt_log_string)
//            when libdlt exports it, completely skipping the
//            start/string/finish dance and the DltContextData scratch.
//
//   Layer 4: dlopen uses RTLD_NOW (catch missing symbols at load time) and
//            honors the IHS_DLT_LIBRARY env var as a soname override.
//
//   Layer 5: A degraded-mode counter tracks emit() calls that were dropped
//            because the bridge had self-disabled, and a one-shot stderr
//            line records the disable reason at startup.
//
// Self-contained ABI types live in ihs::dlt::abi so the new bridge does not
// collide with the legacy shell/logging/dlt/libdlt.h.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ihs::dlt::abi {

inline constexpr int kDltIdSize = 4;

// Mirrors genivi/dlt-daemon DltContext layout.
struct DltContext {
  char id[kDltIdSize];
  std::int32_t pos;
  std::int8_t* p1;
  std::int8_t* p2;
  std::uint8_t count;
};

// DltContextData layout is not stable across libdlt versions or vendor
// forks. The bridge never inspects the bytes — it only passes the buffer
// through libdlt's own entry points — but it must be large enough that no
// reasonable build can write past the end. The upstream struct is ~56 B as
// of libdlt 2.18; 4 KiB leaves ample headroom for vendor additions and
// matches a single page on most architectures.
inline constexpr std::size_t kContextDataSize = 4096;

// Sentinel pattern bracketing the scratch buffer. After every emit(), both
// guards are re-checked; a mismatch means libdlt overran the scratch and
// the bridge must self-disable before the corruption spreads.
inline constexpr std::uint64_t kScratchGuardPattern = 0xDEADBEEFCAFEBABEull;

struct alignas(16) GuardedContextData {
  std::uint64_t head_guard = kScratchGuardPattern;
  unsigned char bytes[kContextDataSize] = {};
  std::uint64_t tail_guard = kScratchGuardPattern;
};

// Major / minor version this bridge was designed against. Passed to
// dlt_check_library_version at load time. Bump in lockstep with any
// upstream version that the bridge has been re-validated against.
inline constexpr const char* kDltExpectedMajor = "2";
inline constexpr const char* kDltExpectedMinor = "18";

}  // namespace ihs::dlt::abi

namespace ihs::dlt {

// Why the bridge ended up disabled, surfaced via disabled_reason() so the
// process can log it (or expose it as a metric) on startup.
enum class DltDisableReason : std::uint8_t {
  Ok = 0,
  LibraryNotFound = 1,
  VersionCheckMissing = 2,
  VersionMismatch = 3,
  RequiredSymbolMissing = 4,
  ScratchOverrun = 5,
};

class LibDltLoader {
 public:
  static LibDltLoader& instance();

  [[nodiscard]] bool available() const noexcept {
    return available_.load(std::memory_order_acquire);
  }

  [[nodiscard]] DltDisableReason disabled_reason() const noexcept {
    return disabled_reason_;
  }

  // Number of emit() calls that were dropped because the bridge had
  // already self-disabled (Layer 5 observability).
  [[nodiscard]] std::uint64_t degraded_drops() const noexcept {
    return degraded_drops_.load(std::memory_order_relaxed);
  }

  // One-shot: register an application id with libdlt (no-op if unavailable).
  bool register_app(const char* app_id, const char* description) noexcept;
  void unregister_app() noexcept;

  // Context registration. Returns true on success or when libdlt is absent
  // (the caller still gets a valid handle for the non-DLT fallback path).
  bool register_context(abi::DltContext* ctx,
                        const char* ctx_id,
                        const char* description) noexcept;
  void unregister_context(abi::DltContext* ctx) noexcept;

  // Emits a single log line. Silently drops if libdlt is unavailable;
  // increments degraded_drops_ in that case.
  void emit(abi::DltContext* ctx, int level, const char* text) noexcept;

 private:
  LibDltLoader();
  ~LibDltLoader();
  LibDltLoader(const LibDltLoader&) = delete;
  LibDltLoader& operator=(const LibDltLoader&) = delete;

  void load();
  void disable(DltDisableReason reason, const char* detail) noexcept;

  void* handle_ = nullptr;
  std::atomic<bool> available_{false};
  DltDisableReason disabled_reason_ = DltDisableReason::Ok;
  std::atomic<std::uint64_t> degraded_drops_{0};

  // Function pointer table — int returns match DltReturnValue.
  int (*fn_check_version_)(const char*, const char*) = nullptr;
  int (*fn_get_version_)(char*, std::size_t) = nullptr;
  int (*fn_register_app_)(const char*, const char*) = nullptr;
  int (*fn_unregister_app_)() = nullptr;
  int (*fn_register_context_)(abi::DltContext*,
                              const char*,
                              const char*) = nullptr;
  int (*fn_unregister_context_)(abi::DltContext*) = nullptr;
  int (*fn_log_write_start_)(abi::DltContext*, void*, int) = nullptr;
  int (*fn_log_write_finish_)(void*) = nullptr;
  int (*fn_log_write_string_)(void*, const char*) = nullptr;

  // Layer 3: optional single-shot string entry point. When non-null,
  // emit() uses it and skips the GuardedContextData scratch path
  // entirely. Symbol name probed: "dlt_log_string".
  int (*fn_log_string_oneshot_)(abi::DltContext*, int, const char*) = nullptr;
};

}  // namespace ihs::dlt
