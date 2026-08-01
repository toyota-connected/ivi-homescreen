// shell/logging/dlt/ring_slot.hpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace ihs::dlt {

inline constexpr std::size_t kSlotTextCapacity = 240;

// RingSlot::flags. A message longer than a slot is pushed as several slots,
// every one but the last carrying kFlagContinues; the drain rejoins them so a
// sink is handed the whole message and emits one line with one prefix. Without
// that a long line arrives split, each piece separately timestamped and
// tagged, and a reader has to reassemble it by hand.
inline constexpr std::uint8_t kFlagContinues = 0x1;

// Fixed-size slot owned by the producer thread's SPSC ring. Cache-line aligned
// to keep producer writes from false-sharing with consumer reads of adjacent
// slots.
struct alignas(64) RingSlot {
  std::uint64_t ts_ns = 0;      // CLOCK_REALTIME captured at push (emit time)
  std::uint32_t ctx_index = 0;  // index into ContextCache
  std::uint32_t sequence = 0;   // monotonically increasing per-ring
  std::uint16_t text_len = 0;
  std::uint8_t level = 0;  // ihs::dlt::LogLevel value
  std::uint8_t flags = 0;  // kFlagContinues
  char text[kSlotTextCapacity] = {};

  [[nodiscard]] const char* message() const noexcept { return text; }
};

static_assert(sizeof(RingSlot) % 64 == 0, "RingSlot must be cache-line sized");

}  // namespace ihs::dlt
