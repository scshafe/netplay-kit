// SipHash-2-4 against the reference vectors from the authors' repository
// (the same set as the paper's appendix example, entry 15).
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "npk/transport/siphash.hpp"

namespace {

constexpr std::uint8_t kVectors[64][8] = {
#include "siphash24_vectors.inc"
};

}  // namespace

int main() {
  npk::transport::SipKey key;
  for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i);

  int failures = 0;
  std::vector<std::uint8_t> msg;
  for (std::size_t len = 0; len < 64; ++len) {
    std::uint64_t want = 0;
    for (int i = 7; i >= 0; --i) want = (want << 8) | kVectors[len][i];
    const std::uint64_t got = npk::transport::siphash24(key, msg);
    if (got != want) {
      std::fprintf(stderr, "length %zu: got %016llx want %016llx\n", len,
                   static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
      ++failures;
    }
    msg.push_back(static_cast<std::uint8_t>(len));
  }

  // The paper's appendix: a 15-byte message gives a129ca6149be45e5.
  std::vector<std::uint8_t> fifteen(15);
  for (std::size_t i = 0; i < 15; ++i) fifteen[i] = static_cast<std::uint8_t>(i);
  if (npk::transport::siphash24(key, fifteen) != 0xa129ca6149be45e5ULL) {
    std::fprintf(stderr, "paper example mismatch\n");
    ++failures;
  }
  return failures == 0 ? 0 : 1;
}
