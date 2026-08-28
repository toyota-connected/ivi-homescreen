// shell/logging/dlt/thread_ring.cpp
#include "thread_ring.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>

namespace {

// Wall-clock nanoseconds, captured at push so a record carries its emit time
// rather than the (later, and per-drain-batch identical) drain time.
std::uint64_t now_realtime_ns() noexcept {
  timespec ts{};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

// Smallest power of two at or above v, never below kMinRingCapacity.
std::size_t round_up_pow2(const std::size_t v) noexcept {
  std::size_t p = ihs::dlt::kMinRingCapacity;
  while (p < v) {
    p <<= 1;
  }
  return p;
}

std::size_t resolve_ring_capacity() noexcept {
  const char* spec = std::getenv("IHS_LOG_RING_CAPACITY");
  if (spec == nullptr || *spec == '\0') {
    return ihs::dlt::kDefaultRingCapacity;
  }
  // strtoull silently wraps a leading '-' (so "-1" would read as the maximum,
  // i.e. the largest allocation this accepts). Reject it before parsing.
  if (*spec == '-') {
    std::fprintf(stderr,
                 "[ihs_log] IHS_LOG_RING_CAPACITY=\"%s\" is not a positive "
                 "integer; using %zu slots\n",
                 spec, ihs::dlt::kDefaultRingCapacity);
    return ihs::dlt::kDefaultRingCapacity;
  }
  char* end = nullptr;
  const unsigned long long v = std::strtoull(spec, &end, 10);
  if (end == nullptr || *end != '\0' || v == 0) {
    std::fprintf(stderr,
                 "[ihs_log] IHS_LOG_RING_CAPACITY=\"%s\" is not a positive "
                 "integer; using %zu slots\n",
                 spec, ihs::dlt::kDefaultRingCapacity);
    return ihs::dlt::kDefaultRingCapacity;
  }

  std::size_t want =
      v > static_cast<unsigned long long>(ihs::dlt::kMaxRingCapacity)
          ? ihs::dlt::kMaxRingCapacity
          : static_cast<std::size_t>(v);
  want = std::max(want, ihs::dlt::kMinRingCapacity);
  const std::size_t capacity = round_up_pow2(want);

  // Say so when the value in force is not the value asked for. A ring silently
  // one power of two off from the request is exactly the kind of thing that
  // gets read back out of a log as evidence.
  if (static_cast<unsigned long long>(capacity) != v) {
    std::fprintf(stderr,
                 "[ihs_log] IHS_LOG_RING_CAPACITY=%s adjusted to %zu slots "
                 "(power of two within [%zu, %zu])\n",
                 spec, capacity, ihs::dlt::kMinRingCapacity,
                 ihs::dlt::kMaxRingCapacity);
  }
  return capacity;
}

}  // namespace

namespace ihs::dlt {

std::size_t ring_capacity() noexcept {
  // Function-local static: resolved on first use, which is the first ring
  // construction, and thread-safe without a lock of our own.
  static const std::size_t capacity = resolve_ring_capacity();
  return capacity;
}

ThreadRing::ThreadRing()
    : capacity_(static_cast<std::uint32_t>(ring_capacity())),
      mask_(capacity_ - 1),
      slots_(std::make_unique<RingSlot[]>(capacity_)) {}

bool ThreadRing::push(std::uint32_t ctx_index,
                      std::uint8_t level,
                      const char* text,
                      std::size_t len,
                      std::uint8_t flags) noexcept {
  const std::uint32_t head = head_.load(std::memory_order_relaxed);
  const std::uint32_t tail = tail_.load(std::memory_order_acquire);

  if (head - tail >= capacity_) {
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  RingSlot& slot = slots_[head & mask_];
  slot.ctx_index = ctx_index;
  slot.level = level;
  slot.flags = flags;
  slot.sequence = head;
  slot.ts_ns = now_realtime_ns();

  const std::size_t copy_len =
      std::min<std::size_t>(len, kSlotTextCapacity - 1);
  if (text != nullptr && copy_len > 0) {
    std::memcpy(slot.text, text, copy_len);
  }
  slot.text[copy_len] = '\0';
  // A clipped record has to say so. Records are capped at
  // IHS_LOG_TEXT_CAPACITY, and a line cut at that boundary reads as a
  // complete one -- while the tail is exactly where a structured payload (a
  // JSON summary, the end of a stack frame) keeps what the reader came for.
  // Stamping the overflow count over the last few characters costs one more
  // character of message than a bare ellipsis and answers "how much did I
  // lose". Only on the truncating branch, which is rare by construction.
  if (len > copy_len) {
    // Wide enough for the longest "...+%zu" a 64-bit size_t can produce (4 +
    // 20 digits + NUL), so the marker itself is never the thing that gets
    // truncated.
    char mark[32];
    const int n = std::snprintf(mark, sizeof(mark), "...+%zu", len - copy_len);
    // snprintf returns the length it WOULD have written, so n >= sizeof(mark)
    // means the marker was itself truncated -- copying n bytes would then run
    // past what was written and drop mark's NUL into the middle of the text.
    // Unreachable with the buffer above; checked because the alternative is a
    // corrupt record if either size ever changes.
    if (n > 0 && static_cast<std::size_t>(n) < sizeof(mark) &&
        static_cast<std::size_t>(n) <= copy_len) {
      std::memcpy(slot.text + copy_len - static_cast<std::size_t>(n), mark,
                  static_cast<std::size_t>(n));
    }
  }
  slot.text_len = static_cast<std::uint16_t>(copy_len);

  head_.store(head + 1, std::memory_order_release);
  return true;
}

const RingSlot* ThreadRing::peek() const noexcept {
  const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
  const std::uint32_t head = head_.load(std::memory_order_acquire);
  if (head == tail) {
    return nullptr;
  }
  return &slots_[tail & mask_];
}

void ThreadRing::pop() noexcept {
  const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
  tail_.store(tail + 1, std::memory_order_release);
}

bool ThreadRing::empty() const noexcept {
  return head_.load(std::memory_order_acquire) ==
         tail_.load(std::memory_order_acquire);
}

std::size_t ThreadRing::pending() const noexcept {
  const std::uint32_t head = head_.load(std::memory_order_acquire);
  const std::uint32_t tail = tail_.load(std::memory_order_acquire);
  return static_cast<std::size_t>(head - tail);
}

}  // namespace ihs::dlt
