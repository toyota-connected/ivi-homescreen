// shell/logging/dlt/ring_slot.hpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace ihs::dlt {

inline constexpr std::size_t kSlotTextCapacity = 240;

// Fixed-size slot owned by the producer thread's SPSC ring. Cache-line aligned
// to keep producer writes from false-sharing with consumer reads of adjacent
// slots.
struct alignas(64) RingSlot {
  std::uint32_t ctx_index = 0;  // index into ContextCache
  std::uint32_t sequence = 0;   // monotonically increasing per-ring
  std::uint16_t text_len = 0;
  std::uint8_t level = 0;  // ihs::dlt::LogLevel value
  std::uint8_t reserved = 0;
  char text[kSlotTextCapacity] = {};

  [[nodiscard]] const char* message() const noexcept { return text; }
};

static_assert(sizeof(RingSlot) % 64 == 0, "RingSlot must be cache-line sized");

}  // namespace ihs::dlt
