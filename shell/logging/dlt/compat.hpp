// shell/logging/dlt/compat.hpp
// ─────────────────────────────────────────────────────────────────────────────
// Provides ihs::xxx aliases that map to the best available C++ implementation.
// Include this instead of <flat_map>, <expected>, <format>, <print> directly.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <memory>
#include <functional>

// ── 1. Map type ───────────────────────────────────────────────────────────────
// ihs::flat_map<K,V> → std::flat_map (C++23) or std::map (C++17/20)

#if defined(IHS_HAS_FLAT_MAP)
#  include <flat_map>
   namespace ihs { template<class K, class V> using flat_map = std::flat_map<K,V>; }
#else
#  include <map>
   namespace ihs { template<class K, class V> using flat_map = std::map<K,V>; }
#endif

// ── 2. Expected / error return ────────────────────────────────────────────────
// ihs::expected<T,E> → std::expected (C++23) or a minimal polyfill (C++17/20)

#if defined(IHS_HAS_STD_EXPECTED)
#  include <expected>
   namespace ihs { template<class T, class E> using expected    = std::expected<T,E>; }
   namespace ihs { template<class E>          using unexpected  = std::unexpected<E>; }
#else
#  include <variant>
#  include <stdexcept>
   namespace ihs {

   // Minimal expected polyfill — value or error, no monadic operations.
   template<class T, class E>
   class expected {
   public:
       expected(T val)              : data_(std::move(val)), has_val_(true)  {}
       expected(std::in_place_t, T val) : data_(std::move(val)), has_val_(true) {}

       static expected make_error(E err) {
           expected x;
           x.has_val_ = false;
           x.err_     = std::move(err);
           return x;
       }

       [[nodiscard]] bool has_value()  const noexcept { return has_val_; }
       [[nodiscard]] T&   value()            { return data_; }
       [[nodiscard]] const T& value()  const { return data_; }
       [[nodiscard]] E    error()      const { return err_; }
       [[nodiscard]] T    value_or(T def) const { return has_val_ ? data_ : def; }

       explicit operator bool() const noexcept { return has_val_; }
       T*       operator->()          { return &data_; }
       const T* operator->()    const { return &data_; }

   private:
       expected() = default;
       T    data_{};
       E    err_{};
       bool has_val_{ false };
   };

   template<class E>
   struct unexpected { E value; };

   } // namespace ihs
#endif

// ── 3. String formatting ──────────────────────────────────────────────────────
// ihs::format_to(buf, size, fmt, ...) → writes into char buf[size]
// Returns number of bytes written (capped at size-1, always null-terminated).

#if defined(IHS_HAS_FORMAT_TO_N)
#  include <format>
   namespace ihs {
   template<class... Args>
   inline std::size_t format_to(char* buf, std::size_t size,
                                 std::format_string<Args...> fmt,
                                 Args&&... args) {
       auto r = std::format_to_n(buf, size - 1,
                                 fmt, std::forward<Args>(args)...);
       *r.out = '\0';
       return static_cast<std::size_t>(r.size);
   }
   } // namespace ihs

#else
   // C++17 fallback: thin wrapper around snprintf.
   namespace ihs {
   template<class... Args>
   inline std::size_t format_to(char* buf, std::size_t size,
                                 const char* fmt, Args&&... args) {
       int n = std::snprintf(buf, size, fmt, std::forward<Args>(args)...);
       if (n < 0) n = 0;
       return static_cast<std::size_t>(n < static_cast<int>(size)
                                       ? n : size - 1);
   }
   } // namespace ihs
#endif

// ── 4. Stderr printing ────────────────────────────────────────────────────────
// ihs::println(fmt, ...) → std::println(stderr, ...) or fprintf(stderr, ...)

#if defined(IHS_HAS_STD_PRINT)
#  include <print>
   namespace ihs {
   template<class... Args>
   inline void println(std::format_string<Args...> fmt, Args&&... args) {
       std::println(stderr, fmt, std::forward<Args>(args)...);
   }
   } // namespace ihs
#else
   namespace ihs {
   template<class... Args>
   inline void println(const char* fmt, Args&&... args) {
       std::fprintf(stderr, fmt, std::forward<Args>(args)...);
       std::fputc('\n', stderr);
   }
   } // namespace ihs
#endif

// ── 5. Thread types ───────────────────────────────────────────────────────────
// ihs::jthread / ihs::stop_token → std::jthread (C++20) or a polyfill (C++17)

#if defined(IHS_HAS_STD_JTHREAD)
#  include <thread>
   namespace ihs {
   using jthread    = std::jthread;
   using stop_token = std::stop_token;
   } // namespace ihs

#else
#  include <thread>
#  include <atomic>
#  include <tuple>
   namespace ihs {

   // Minimal stop_source / stop_token polyfill
   class stop_source;

   class stop_token {
   public:
       stop_token() = default;
       [[nodiscard]] bool stop_requested() const noexcept {
           return flag_ && flag_->load(std::memory_order_acquire);
       }
   private:
       friend class stop_source;
       explicit stop_token(std::shared_ptr<std::atomic<bool>> f)
           : flag_(std::move(f)) {}
       std::shared_ptr<std::atomic<bool>> flag_;
   };

   class stop_source {
   public:
       stop_source()
           : flag_(std::make_shared<std::atomic<bool>>(false)) {}
       void     request_stop() noexcept {
           flag_->store(true, std::memory_order_release);
       }
       stop_token get_token() const noexcept {
           return stop_token{flag_};
       }
   private:
       std::shared_ptr<std::atomic<bool>> flag_;
   };

   // jthread polyfill: thread that receives a stop_token as first argument
   // and whose destructor automatically requests stop + joins.
   class jthread {
   public:
       jthread() = default;

       template<class F, class... Args>
       explicit jthread(F&& f, Args&&... args) {
           auto token = source_.get_token();
           thread_ = std::thread(
               [func  = std::forward<F>(f),
                tok   = std::move(token),
                targs = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                   std::apply([&](auto&&... a) {
                       func(std::move(tok),
                            std::forward<decltype(a)>(a)...);
                   }, std::move(targs));
               });
       }

       ~jthread() {
           request_stop();
           if (thread_.joinable()) thread_.join();
       }

       jthread(const jthread&)            = delete;
       jthread& operator=(const jthread&) = delete;
       jthread(jthread&&)                 = default;
       jthread& operator=(jthread&&)      = default;

       void request_stop() noexcept { source_.request_stop(); }
       bool joinable()     const noexcept { return thread_.joinable(); }
       void join()         { thread_.join(); }

   private:
       stop_source  source_;
       std::thread  thread_;
   };

   } // namespace ihs
#endif

// ── 6. Format string portability macro ───────────────────────────────────────
// IHS_FMT(fmt_str) produces the correct format literal for the active backend.

#if defined(IHS_HAS_FORMAT_TO_N)
#  define IHS_FMT(s)       s
#  define IHS_FMT_C17(s)   s
#else
#  define IHS_FMT(s)       s
#  define IHS_FMT_C17(s)   s
#endif
