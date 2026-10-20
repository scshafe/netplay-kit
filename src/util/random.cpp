#include "npk/util/random.hpp"

#include <random>

namespace npk::util {

void random_bytes(std::span<std::uint8_t> out) {
  static thread_local std::random_device rd;
  std::size_t i = 0;
  while (i < out.size()) {
    std::uint32_t v = rd();
    for (int k = 0; k < 4 && i < out.size(); ++k, ++i) {
      out[i] = static_cast<std::uint8_t>(v);
      v >>= 8;
    }
  }
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
  static const char digits[] = "0123456789abcdef";
  std::string s;
  s.reserve(bytes.size() * 2);
  for (const auto b : bytes) {
    s += digits[b >> 4];
    s += digits[b & 15];
  }
  return s;
}

std::string random_hex(std::size_t n) {
  std::string raw(n, '\0');
  random_bytes({reinterpret_cast<std::uint8_t*>(raw.data()), n});
  return to_hex({reinterpret_cast<const std::uint8_t*>(raw.data()), n});
}

std::string random_id(std::size_t len) {
  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";  // 36
  std::string s(len, '\0');
  random_bytes({reinterpret_cast<std::uint8_t*>(s.data()), len});
  for (char& c : s) c = alphabet[static_cast<std::uint8_t>(c) % 36];  // a negligible bias, fine for identifiers
  return s;
}

}  // namespace npk::util
