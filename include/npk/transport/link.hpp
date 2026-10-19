#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "npk/transport/address.hpp"

namespace npk::transport {

// Where datagrams go in and out. The transport talks to a Link, never to a
// socket, so the same code runs over UDP (UdpSocket) and over the in-process
// lossy-link simulator (SimLink). Both calls are non-blocking.
class Link {
 public:
  virtual ~Link() = default;

  virtual Address local_address() const = 0;

  // Sends one datagram. UDP gives no delivery promise: true means it was handed
  // to the network, false means it was refused (too large, no buffer space).
  virtual bool send(const Address& to, std::span<const std::uint8_t> data) = 0;

  // Takes the next received datagram, if any, into buf and returns its size.
  // A datagram longer than buf is cut to buf.size(), so a caller that wants to
  // see oversize datagrams passes a buffer of kMaxDatagram + 1.
  virtual std::optional<std::size_t> receive(Address& from, std::span<std::uint8_t> buf) = 0;
};

}  // namespace npk::transport
