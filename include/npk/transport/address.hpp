#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace npk::transport {

// An IP address and UDP port. IPv4 is stored v4-mapped (::ffff:a.b.c.d), so one
// type, ordering and hash serve both families.
struct Address {
  std::array<std::uint8_t, 16> ip{};
  std::uint16_t port = 0;

  static Address v4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d,
                    std::uint16_t port) noexcept;
  static Address any_v4(std::uint16_t port) noexcept { return v4(0, 0, 0, 0, port); }
  static Address loopback_v4(std::uint16_t port) noexcept { return v4(127, 0, 0, 1, port); }
  static Address any_v6(std::uint16_t port) noexcept;
  static Address loopback_v6(std::uint16_t port) noexcept;
  // A literal address ("192.0.2.1", "::1"); no name resolution.
  static std::optional<Address> parse(std::string_view host, std::uint16_t port);

  bool is_v4() const noexcept;
  std::string to_string() const;  // "192.0.2.1:7000", "[::1]:7000"

  friend auto operator<=>(const Address&, const Address&) = default;
};

}  // namespace npk::transport

template <>
struct std::hash<npk::transport::Address> {
  std::size_t operator()(const npk::transport::Address& a) const noexcept {
    std::uint64_t h = 1469598103934665603ULL;  // FNV-1a; not security relevant
    for (auto b : a.ip) h = (h ^ b) * 1099511628211ULL;
    h = (h ^ a.port) * 1099511628211ULL;
    return static_cast<std::size_t>(h);
  }
};
