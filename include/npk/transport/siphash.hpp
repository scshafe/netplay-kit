#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace npk::transport {

using SipKey = std::array<std::uint8_t, 16>;

// SipHash-2-4 with a 64-bit output (Aumasson and Bernstein). It is the keyed
// hash behind handshake cookies: a short-input PRF, not a general-purpose MAC.
// Tested against the 64 reference vectors in tests/siphash24_vectors.inc.
std::uint64_t siphash24(const SipKey& key, std::span<const std::uint8_t> data) noexcept;

}  // namespace npk::transport
