#pragma once

#include <memory>
#include <string>

#include "npk/transport/link.hpp"

namespace npk::transport {

// A non-blocking UDP socket over the POSIX socket API (Linux and macOS). An IPv4
// bind address gives an IPv4 socket; an IPv6 one gives an IPv6 socket that also
// takes IPv4 (dual-stack) where the OS allows it.
class UdpSocket final : public Link {
 public:
  // Binds to local (port 0 picks one; local_address() tells which). On failure
  // returns null and describes why in *error when given.
  static std::unique_ptr<UdpSocket> bind(const Address& local, std::string* error = nullptr);

  ~UdpSocket() override;
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  Address local_address() const override { return local_; }
  bool send(const Address& to, std::span<const std::uint8_t> data) override;
  std::optional<std::size_t> receive(Address& from, std::span<std::uint8_t> buf) override;

  // Blocks until a datagram can be read or the timeout passes. True if readable.
  bool wait_readable(int timeout_ms);

 private:
  UdpSocket(int fd, Address local, bool v6) : fd_(fd), local_(local), v6_(v6) {}
  int fd_;
  Address local_;
  bool v6_;
};

}  // namespace npk::transport
