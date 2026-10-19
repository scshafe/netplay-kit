#pragma once

// Private: where the handshake gets its unpredictable numbers. With seed 0 they
// come from the OS (std::random_device); a non-zero seed makes them repeatable
// for tests.

#include <cstdint>
#include <random>

#include "npk/transport/siphash.hpp"

namespace npk::transport {

class Entropy {
 public:
  explicit Entropy(std::uint64_t seed) : seeded_(seed != 0), rng_(seed) {}

  std::uint64_t next() {
    if (seeded_) return rng_();
    return (static_cast<std::uint64_t>(rd_()) << 32) | rd_();
  }

  SipKey key() {
    SipKey k;
    for (int i = 0; i < 2; ++i) {
      const std::uint64_t v = next();
      for (int j = 0; j < 8; ++j) k[static_cast<std::size_t>(i * 8 + j)] = static_cast<std::uint8_t>(v >> (8 * j));
    }
    return k;
  }

 private:
  bool seeded_;
  std::mt19937_64 rng_;
  std::random_device rd_;
};

}  // namespace npk::transport
