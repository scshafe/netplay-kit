#include "npk/transport/address.hpp"

#include <arpa/inet.h>

#include <cstring>

namespace npk::transport {

Address Address::v4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d,
                    std::uint16_t port) noexcept {
  Address r;
  r.ip[10] = 0xff;
  r.ip[11] = 0xff;
  r.ip[12] = a;
  r.ip[13] = b;
  r.ip[14] = c;
  r.ip[15] = d;
  r.port = port;
  return r;
}

Address Address::any_v6(std::uint16_t port) noexcept {
  Address r;
  r.port = port;
  return r;
}

Address Address::loopback_v6(std::uint16_t port) noexcept {
  Address r;
  r.ip[15] = 1;
  r.port = port;
  return r;
}

std::optional<Address> Address::parse(std::string_view host, std::uint16_t port) {
  const std::string s(host);
  Address r;
  r.port = port;
  in_addr a4;
  if (inet_pton(AF_INET, s.c_str(), &a4) == 1) {
    r.ip[10] = 0xff;
    r.ip[11] = 0xff;
    std::memcpy(&r.ip[12], &a4, 4);
    return r;
  }
  in6_addr a6;
  if (inet_pton(AF_INET6, s.c_str(), &a6) == 1) {
    std::memcpy(r.ip.data(), &a6, 16);
    return r;
  }
  return std::nullopt;
}

bool Address::is_v4() const noexcept {
  static constexpr std::array<std::uint8_t, 12> prefix{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
  return std::memcmp(ip.data(), prefix.data(), prefix.size()) == 0;
}

std::string Address::to_string() const {
  char buf[INET6_ADDRSTRLEN] = {};
  if (is_v4()) {
    inet_ntop(AF_INET, &ip[12], buf, sizeof buf);
    return std::string(buf) + ":" + std::to_string(port);
  }
  inet_ntop(AF_INET6, ip.data(), buf, sizeof buf);
  return "[" + std::string(buf) + "]:" + std::to_string(port);
}

}  // namespace npk::transport
