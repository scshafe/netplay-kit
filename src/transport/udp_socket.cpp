#include "npk/transport/udp_socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace npk::transport {
namespace {

void fail(std::string* error, const char* what) {
  if (error) *error = std::string(what) + ": " + std::strerror(errno);
}

socklen_t to_sockaddr(const Address& a, bool v6, sockaddr_storage& ss) {
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

Address from_sockaddr(const sockaddr_storage& ss) {
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

}  // namespace

std::unique_ptr<UdpSocket> UdpSocket::bind(const Address& local, std::string* error) {
  const bool v6 = !local.is_v4();
  const int fd = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    fail(error, "socket");
    return nullptr;
  }
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    fail(error, "fcntl(O_NONBLOCK)");
    ::close(fd);
    return nullptr;
  }
  if (v6) {
    const int off = 0;  // best effort: one socket for both families
    ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
  }
  sockaddr_storage ss;
  const socklen_t len = to_sockaddr(local, v6, ss);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&ss), len) < 0) {
    fail(error, "bind");
    ::close(fd);
    return nullptr;
  }
  sockaddr_storage bound;
  socklen_t blen = sizeof bound;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen) < 0) {
    fail(error, "getsockname");
    ::close(fd);
    return nullptr;
  }
  return std::unique_ptr<UdpSocket>(new UdpSocket(fd, from_sockaddr(bound), v6));
}

UdpSocket::~UdpSocket() { ::close(fd_); }

bool UdpSocket::send(const Address& to, std::span<const std::uint8_t> data) {
  if (!v6_ && !to.is_v4()) return false;  // an IPv4 socket cannot reach IPv6
  sockaddr_storage ss;
  const socklen_t len = to_sockaddr(to, v6_, ss);
  for (;;) {
    const ssize_t n = ::sendto(fd_, data.data(), data.size(), 0,
                               reinterpret_cast<const sockaddr*>(&ss), len);
    if (n >= 0) return static_cast<std::size_t>(n) == data.size();
    if (errno != EINTR) return false;  // full buffer, no route, too large: UDP just drops
  }
}

std::optional<std::size_t> UdpSocket::receive(Address& from, std::span<std::uint8_t> buf) {
  for (;;) {
    sockaddr_storage ss;
    socklen_t len = sizeof ss;
    const ssize_t n = ::recvfrom(fd_, buf.data(), buf.size(), 0,
                                 reinterpret_cast<sockaddr*>(&ss), &len);
    if (n >= 0) {
      from = from_sockaddr(ss);
      return static_cast<std::size_t>(n);
    }
    // An earlier send bounced (ICMP error) or a signal arrived: not a datagram.
    if (errno == EINTR || errno == ECONNREFUSED || errno == ECONNRESET) continue;
    return std::nullopt;  // EAGAIN: nothing waiting
  }
}

bool UdpSocket::wait_readable(int timeout_ms) {
  pollfd p{fd_, POLLIN, 0};
  for (;;) {
    const int r = ::poll(&p, 1, timeout_ms);
    if (r >= 0) return r > 0 && (p.revents & POLLIN);
    if (errno != EINTR) return false;
  }
}

}  // namespace npk::transport
