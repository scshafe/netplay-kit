#pragma once

// Unpredictable bytes from the OS (std::random_device), for tokens, secrets and
// identifiers. Not for simulations, which must be deterministic.

#include <cstdint>
#include <span>
#include <string>

namespace npk::util {

void random_bytes(std::span<std::uint8_t> out);
std::string to_hex(std::span<const std::uint8_t> bytes);
// `n` random bytes as 2n lowercase hex characters.
std::string random_hex(std::size_t n);
// `len` characters of [a-z0-9].
std::string random_id(std::size_t len);

}  // namespace npk::util
