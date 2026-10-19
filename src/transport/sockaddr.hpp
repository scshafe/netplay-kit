#pragma once

// Private: conversions between Address and the OS's socket addresses, shared by
// the UDP socket and the TCP code.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>

#include "npk/transport/address.hpp"

namespace npk::transport {

inline socklen_t to_sockaddr(const Address& a, bool v6, sockaddr_storage& ss) {
  std::memset(&ss, 0, sizeof ss);
  if (v6) {
    auto& s = reinterpret_cast<sockaddr_in6&>(ss);
    s.sin6_family = AF_INET6;
    s.sin6_port = htons(a.port);
    std::memcpy(&s.sin6_addr, a.ip.data(), 16);
    return sizeof(sockaddr_in6);
  }
  auto& s = reinterpret_cast<sockaddr_in&>(ss);
  s.sin_family = AF_INET;
  s.sin_port = htons(a.port);
  std::memcpy(&s.sin_addr, &a.ip[12], 4);
  return sizeof(sockaddr_in);
}

inline Address from_sockaddr(const sockaddr_storage& ss) {
  Address a;
  if (ss.ss_family == AF_INET6) {
    const auto& s = reinterpret_cast<const sockaddr_in6&>(ss);
    std::memcpy(a.ip.data(), &s.sin6_addr, 16);
    a.port = ntohs(s.sin6_port);
  } else {
    const auto& s = reinterpret_cast<const sockaddr_in&>(ss);
    a.ip[10] = 0xff;
    a.ip[11] = 0xff;
    std::memcpy(&a.ip[12], &s.sin_addr, 4);
    a.port = ntohs(s.sin_port);
  }
  return a;
}

}  // namespace npk::transport
