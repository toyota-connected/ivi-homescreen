// shell/logging/dlt/thread_ring.hpp
#pragma once

#include "ring_slot.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ihs::dlt {

// Ring depth, in slots. A RingSlot is cache-line padded around a 240-byte
// text buffer, so the default costs ~80 KiB per logging thread.
//
// The default is a footprint/loss compromise tuned for steady-state logging,
// and a diagnostic run is not steady-state: a burst that outruns the drain
// worker -- scene load, asset streaming, anything that logs per item at debug
// or verbose level -- sheds records, and what survives reads like a complete
// log rather than one with holes in it. Depth is therefore configurable, so
// such a run can pay memory the shipped configuration does not have to.
inline constexpr std::size_t kDefaultRingCapacity = 256;
inline constexpr std::size_t kMinRingCapacity = 16;
// 64 Ki slots is ~20 MiB per logging thread -- already generous for a
// diagnostic run, and having a ceiling keeps a fat-fingered value from turning
// the first log call into an allocation failure.
inline constexpr std::size_t kMaxRingCapacity = 64 * 1024;

// Ring depth for this process: IHS_LOG_RING_CAPACITY when set, otherwise
// kDefaultRingCapacity. Always a power of two -- a requested value is clamped
// to [kMinRingCapacity, kMaxRingCapacity] and then rounded up -- so the index
// mask stays valid.
//
// Resolved once, on first use, and constant for the life of the process.
// Rings are pooled and reused across threads, so a depth that could change
// under a running process would leave rings of assorted depths behind and make
// "how much can this thread buffer" unanswerable.
[[nodiscard]] std::size_t ring_capacity() noexcept;

// Single-producer (owning thread), single-consumer (worker) ring.
// Producer writes are lock-free and wait-free; the consumer drains by peeking
// and advancing the tail. Overflows are counted, not blocked.
class ThreadRing {
 public:
  // Allocates its slot array, so unlike the old inline-array ring this can
  // throw. Rings are only ever created through RingRegistry::acquire_ring(),
  // which already reaches the caller through a throwing `new ThreadRing()`.
  ThreadRing();

  ThreadRing(const ThreadRing&) = delete;
  ThreadRing& operator=(const ThreadRing&) = delete;

  // Producer side — called only from the ring's owning thread.
  bool push(std::uint32_t ctx_index,
            std::uint8_t level,
            const char* text,
            std::size_t len,
            std::uint8_t flags = 0) noexcept;

  // Free slots, from the producer's side. Safe to act on: the consumer only
  // ever frees more, so a count taken here cannot shrink before the producer
  // uses it. Lets a caller splitting one message across slots check up front
  // that the whole sequence fits, rather than discover halfway that it does
  // not and leave an unterminated run behind.
  [[nodiscard]] std::size_t space() const noexcept {
    const std::uint32_t head = head_.load(std::memory_order_relaxed);
    const std::uint32_t tail = tail_.load(std::memory_order_acquire);
    const std::uint32_t used = head - tail;
    return used >= capacity_ ? 0 : capacity_ - used;
  }

  // Consumer side — called only from the worker thread.
  [[nodiscard]] const RingSlot* peek() const noexcept;
  void pop() noexcept;

  // Reassembly buffer for a message split across slots. Consumer-side only:
  // the worker owns it, so it needs no synchronization, and it lives here
  // rather than in the worker because a drain can end mid-sequence and the
  // partial has to survive until the next pass over this ring.
  //
  // Capped so a producer that never terminates a sequence cannot grow it
  // without bound; past the cap the tail is dropped, which is the same outcome
  // as the old per-slot clipping and strictly better than exhausting memory.
  [[nodiscard]] const std::string& partial() const noexcept { return partial_; }

  // The emit time of the piece that started the partial. A rejoined message
  // belongs at the instant it was logged, not at the instant its last piece
  // landed -- otherwise a long message sorts after short ones that were logged
  // while it was still being written.
  [[nodiscard]] std::uint64_t partial_ts_ns() const noexcept {
    return partial_ts_ns_;
  }
  void begin_partial(std::uint64_t ts_ns) noexcept { partial_ts_ns_ = ts_ns; }
  void append_partial(const char* text, std::size_t len) {
    if (text == nullptr || dropped_partial_) {
      return;
    }
    // Mark the clip where it happens rather than on a later call: the piece
    // that overflows may be the last one, and then no later call comes. A
    // reader cannot otherwise tell a complete message from one that hit the
    // ceiling. push() marks its own clip the same way, for the message it is
    // handed whole when a split would not fit the ring.
    const std::size_t room =
        kMaxText > partial_.size() ? kMaxText - partial_.size() : 0;
    if (len <= room) {
      partial_.append(text, len);
      return;
    }
    partial_.append(text, room);
    partial_.append(kCapMark);  // room was reserved for it; the cap holds
    dropped_partial_ = true;
  }
  void clear_partial() noexcept {
    partial_.clear();
    dropped_partial_ = false;
    partial_ts_ns_ = 0;
  }

  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::size_t pending() const noexcept;

  [[nodiscard]] std::uint64_t dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
  }

  // Ownership pool (see RingRegistry). A ring is owned by exactly one producer
  // thread at a time. try_claim() takes an unowned ring; release() (run from
  // the owner's thread-exit) returns it to the pool for a later thread to
  // reuse, so total rings stay bounded by peak concurrent logging threads
  // rather than the count of threads that ever logged. A fresh ring is born
  // owned by its creator. The ring is never freed and stays registered, so
  // the worker keeps draining any residual slots across an ownership change;
  // SPSC holds because release() runs only after the old owner's last push.
  [[nodiscard]] bool try_claim() noexcept {
    bool expected = false;
    return in_use_.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel, std::memory_order_relaxed);
  }
  void release() noexcept { in_use_.store(false, std::memory_order_release); }

  // Registry linkage — touched only under RingRegistry's ownership.
  ThreadRing* next = nullptr;

 private:
  // kMaxPartial bounds the buffer; kMaxText is what a message may occupy in
  // it, the difference reserved so the marker always fits without pushing the
  // buffer past its own cap.
  static constexpr std::string_view kCapMark = "...[log record capped]";
  static constexpr std::size_t kMaxPartial = 64 * 1024;
  static constexpr std::size_t kMaxText = kMaxPartial - kCapMark.size();
  std::string partial_;
  std::uint64_t partial_ts_ns_ = 0;  // emit time of the piece that started it
  bool dropped_partial_ = false;     // cap hit; marker already appended

 public:
 private:
  // Depth and index mask for this ring, fixed at construction from
  // ring_capacity(). Members rather than constants because the depth is now a
  // process setting resolved at runtime. Both are read-only afterwards, so
  // sharing a cache line with in_use_ costs nothing: in_use_ is written only
  // when a thread claims or releases the ring.
  const std::uint32_t capacity_;
  const std::uint32_t mask_;

  std::atomic<bool> in_use_{true};  // born claimed by its creating thread
  alignas(64) std::atomic<std::uint32_t> head_{0};  // producer writes
  alignas(64) std::atomic<std::uint32_t> tail_{0};  // consumer reads
  alignas(64) std::atomic<std::uint64_t> dropped_{0};
  // Heap-allocated rather than an inline array so the depth can be chosen at
  // runtime. RingSlot is alignas(64), so array new returns cache-line aligned
  // storage and the inline array's false-sharing property is preserved. In
  // practice never freed -- rings outlive their owning thread by design (see
  // RingRegistry) -- but owned here so the type stays honest.
  std::unique_ptr<RingSlot[]> slots_;
};

}  // namespace ihs::dlt
