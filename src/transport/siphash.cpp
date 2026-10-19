#include "npk/transport/siphash.hpp"

#include <bit>

namespace npk::transport {
namespace {

std::uint64_t load_le64(const std::uint8_t* p) noexcept {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

struct State {
  std::uint64_t v0, v1, v2, v3;

  void round() noexcept {
    v0 += v1;
    v1 = std::rotl(v1, 13);
    v1 ^= v0;
    v0 = std::rotl(v0, 32);
    v2 += v3;
    v3 = std::rotl(v3, 16);
    v3 ^= v2;
    v0 += v3;
    v3 = std::rotl(v3, 21);
    v3 ^= v0;
    v2 += v1;
    v1 = std::rotl(v1, 17);
    v1 ^= v2;
    v2 = std::rotl(v2, 32);
  }
};

}  // namespace

std::uint64_t siphash24(const SipKey& key, std::span<const std::uint8_t> data) noexcept {
  const std::uint64_t k0 = load_le64(key.data());
  const std::uint64_t k1 = load_le64(key.data() + 8);
  State s{k0 ^ 0x736f6d6570736575ULL, k1 ^ 0x646f72616e646f6dULL,
          k0 ^ 0x6c7967656e657261ULL, k1 ^ 0x7465646279746573ULL};

  const std::size_t full = data.size() & ~static_cast<std::size_t>(7);
  for (std::size_t i = 0; i < full; i += 8) {
    const std::uint64_t m = load_le64(data.data() + i);
    s.v3 ^= m;
    s.round();
    s.round();
    s.v0 ^= m;
  }

  // The last block holds the remaining 0..7 bytes and the length mod 256 on top.
  std::uint64_t last = static_cast<std::uint64_t>(data.size() & 0xff) << 56;
  for (std::size_t i = 0; i < data.size() - full; ++i) {
    last |= static_cast<std::uint64_t>(data[full + i]) << (8 * i);
  }
  s.v3 ^= last;
  s.round();
  s.round();
  s.v0 ^= last;

  s.v2 ^= 0xff;
  s.round();
  s.round();
  s.round();
  s.round();
  return s.v0 ^ s.v1 ^ s.v2 ^ s.v3;
}

}  // namespace npk::transport
