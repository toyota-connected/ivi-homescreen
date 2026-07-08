// shell/logging/dlt/log_level.hpp
#pragma once

#include <cstdint>

namespace ihs::dlt {

enum class LogLevel : std::uint8_t {
  Off = 0,
  Fatal = 1,
  Error = 2,
  Warn = 3,
  Info = 4,
  Debug = 5,
  Verbose = 6,
};

[[nodiscard]] constexpr int to_dlt_level(LogLevel l) noexcept {
  return static_cast<int>(l);
}

}  // namespace ihs::dlt
