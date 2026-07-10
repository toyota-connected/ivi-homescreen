// shell/logging/dlt/compat.hpp
// ─────────────────────────────────────────────────────────────────────────────
// Provides ihs::xxx aliases that map to the best available C++ implementation.
// Include this instead of <flat_map>, <expected>, <format>, <print> directly.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

// ── 1. Map type
// ───────────────────────────────────────────────────────────────
// ihs::flat_map<K,V> → std::flat_map (C++23) or std::map (C++17/20)

#if defined(IHS_HAS_FLAT_MAP)
#include <flat_map>
namespace ihs {
template <class K, class V>
using flat_map = std::flat_map<K, V>;
}
#else
#include <map>
namespace ihs {
template <class K, class V>
using flat_map = std::map<K, V>;
}
#endif

// ── 2. Expected / error return
// ──────────────────────────────────────────────── ihs::expected<T,E> →
// std::expected (C++23) or a minimal polyfill (C++17/20)

#if defined(IHS_HAS_STD_EXPECTED)
#include <expected>
namespace ihs {
template <class T, class E>
using expected = std::expected<T, E>;
}
namespace ihs {
template <class E>
using unexpected = std::unexpected<E>;
}
namespace ihs {
template <class T, class E>
inline expected<T, E> unexpect(E err) {
  return std::unexpected<E>(std::move(err));
}
}  // namespace ihs
#else
#include <stdexcept>
#include <variant>
namespace ihs {

// Minimal expected polyfill — value or error, no monadic operations.
template <class T, class E>
class expected {
 public:
  expected(T val) : data_(std::move(val)), has_val_(true) {}
  expected(std::in_place_t, T val) : data_(std::move(val)), has_val_(true) {}

  static expected make_error(E err) {
    expected x;
    x.has_val_ = false;
    x.err_ = std::move(err);
    return x;
  }

  [[nodiscard]] bool has_value() const noexcept { return has_val_; }
  [[nodiscard]] T& value() { return data_; }
  [[nodiscard]] const T& value() const { return data_; }
  [[nodiscard]] E error() const { return err_; }
  [[nodiscard]] T value_or(T def) const { return has_val_ ? data_ : def; }

  explicit operator bool() const noexcept { return has_val_; }
  T* operator->() { return &data_; }
  const T* operator->() const { return &data_; }

 private:
  expected() = default;
  T data_{};
  E err_{};
  bool has_val_{false};
};

template <class E>
struct unexpected {
  E value;
};

template <class T, class E>
inline expected<T, E> unexpect(E err) {
  return expected<T, E>::make_error(std::move(err));
}

}  // namespace ihs
#endif

// ── 3. String formatting
// ──────────────────────────────────────────────────────
// ihs::format_to(buf, size, fmt, ...) → writes into char buf[size], returns
// bytes written (capped at size-1, always null-terminated). Defined once in
// the public header so the bridge and the shell's logger share it.

#include "ihs/format.h"

// ── 4. Stderr printing
// ──────────────────────────────────────────────────────── ihs::println(fmt,
// ...) → std::println(stderr, ...) or fprintf(stderr, ...)

#if defined(IHS_HAS_STD_PRINT)
#include <print>
namespace ihs {
template <class... Args>
inline void println(std::format_string<Args...> fmt, Args&&... args) {
  std::println(stderr, fmt, std::forward<Args>(args)...);
}
}  // namespace ihs
#else
namespace ihs {
template <class... Args>
inline void println(const char* fmt, Args&&... args) {
  std::fprintf(stderr, fmt, std::forward<Args>(args)...);
  std::fputc('\n', stderr);
}
}  // namespace ihs
#endif

// ── 5. Thread types
// ─────────────────────────────────────────────────────────── ihs::jthread /
// ihs::stop_token → std::jthread (C++20) or a polyfill (C++17)

#if defined(IHS_HAS_STD_JTHREAD)
#include <thread>
namespace ihs {
using jthread = std::jthread;
using stop_token = std::stop_token;
}  // namespace ihs

#else
#include <atomic>
#include <thread>
#include <tuple>
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
  stop_source() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
  void request_stop() noexcept {
    // flag_ may be null after this source has been moved from.
    if (flag_)
      flag_->store(true, std::memory_order_release);
  }
  stop_token get_token() const noexcept { return stop_token{flag_}; }

 private:
  std::shared_ptr<std::atomic<bool>> flag_;
};

// Erased holder used by the jthread polyfill below. The std::thread is
// constructed with a free-function pointer (run_holder) plus a
// shared_ptr<HolderBase>; no closure type is ever passed to std::thread.
//
// Why this matters: libstdc++14 + Clang-17 + -std=c++23 has a
// non-SFINAE-friendly tuple check inside std::thread's ctor that fires
// on closure types. Wrapping the user callable in a virtual call behind
// a function pointer keeps std::thread's _Invoker tuple holding only
// (function-pointer, shared_ptr) — neither of which is a lambda.
namespace detail {
struct JThreadHolderBase {
  stop_token tok;
  virtual ~JThreadHolderBase() = default;
  virtual void run() = 0;
};

template <class F, class Tup>
struct JThreadHolder final : JThreadHolderBase {
  F func;
  Tup args;
  JThreadHolder(F f, Tup a, stop_token t)
      : func(std::move(f)), args(std::move(a)) {
    tok = std::move(t);
  }
  void run() override {
    std::apply(
        [&](auto&&... a) {
          func(std::move(tok), std::forward<decltype(a)>(a)...);
        },
        std::move(args));
  }
};

inline void run_jthread_holder(std::shared_ptr<JThreadHolderBase> holder) {
  holder->run();
}
}  // namespace detail

// jthread polyfill: thread that receives a stop_token as first argument
// and whose destructor automatically requests stop + joins.
class jthread {
 public:
  jthread() = default;

  template <class F, class... Args>
  explicit jthread(F&& f, Args&&... args) {
    using FDecay = typename std::decay<F>::type;
    using TupT = std::tuple<typename std::decay<Args>::type...>;
    using HolderT = detail::JThreadHolder<FDecay, TupT>;

    auto holder = std::make_shared<HolderT>(std::forward<F>(f),
                                            TupT(std::forward<Args>(args)...),
                                            source_.get_token());

    // Pass a free function pointer + shared_ptr (both non-closure
    // types) to std::thread, sidestepping the libstdc++14 lambda
    // ctor SFINAE bug.
    thread_ = std::thread(&detail::run_jthread_holder,
                          std::shared_ptr<detail::JThreadHolderBase>(holder));
  }

  ~jthread() {
    request_stop();
    if (thread_.joinable())
      thread_.join();
  }

  jthread(const jthread&) = delete;
  jthread& operator=(const jthread&) = delete;
  jthread(jthread&&) = default;
  jthread& operator=(jthread&&) = default;

  void request_stop() noexcept { source_.request_stop(); }
  bool joinable() const noexcept { return thread_.joinable(); }
  void join() { thread_.join(); }

 private:
  stop_source source_;
  std::thread thread_;
};

}  // namespace ihs
#endif

// ── 6. Format string portability macro ───────────────────────────────────────
// IHS_FMT(fmt_str) produces the correct format literal for the active backend.

#if defined(IHS_HAS_FORMAT_TO_N)
#define IHS_FMT(s) s
#define IHS_FMT_C17(s) s
#else
#define IHS_FMT(s) s
#define IHS_FMT_C17(s) s
#endif
