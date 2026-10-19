#pragma once

#include <chrono>
#include <cstdint>

namespace npk::transport {

// Microseconds on a monotonic clock. The transport never reads a clock itself:
// its owner passes `now` in, so tests drive it with simulated time.
using Micros = std::int64_t;

inline Micros steady_now_us() noexcept {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace npk::transport
