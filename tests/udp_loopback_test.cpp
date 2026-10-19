// Real UDP sockets over loopback, and the address type.
#include <chrono>
#include <thread>
#include <vector>

#include "check.hpp"
#include "npk/transport/udp_socket.hpp"
#include "npk/transport/wire.hpp"

using namespace npk::transport;

namespace {

void addresses() {
  const auto a = Address::parse("192.0.2.7", 7000);
  CHECK(a && a->is_v4() && a->to_string() == "192.0.2.7:7000");
  CHECK(*a == Address::v4(192, 0, 2, 7, 7000));
  const auto b = Address::parse("::1", 80);
  CHECK(b && !b->is_v4() && b->to_string() == "[::1]:80");
  CHECK(*b == Address::loopback_v6(80));
  CHECK(!Address::parse("example.com", 1));
  CHECK(!Address::parse("300.1.1.1", 1));
  CHECK(Address::v4(1, 2, 3, 4, 5) < Address::v4(1, 2, 3, 4, 6));
  CHECK(std::hash<Address>{}(*a) == std::hash<Address>{}(Address::v4(192, 0, 2, 7, 7000)));
}

// Waits for one datagram, up to a second.
std::optional<std::size_t> receive_soon(UdpSocket& s, Address& from, std::span<std::uint8_t> buf) {
  for (int i = 0; i < 10; ++i) {
    s.wait_readable(100);
    if (auto n = s.receive(from, buf)) return n;
  }
  return std::nullopt;
}

void v4_exchange() {
  std::string err;
  auto a = UdpSocket::bind(Address::loopback_v4(0), &err);
  auto b = UdpSocket::bind(Address::loopback_v4(0), &err);
  CHECK(a && b);
  CHECK(a->local_address().port != 0 && a->local_address().port != b->local_address().port);

  std::vector<std::uint8_t> buf(kMaxDatagram + 1);
  Address from;
  CHECK(!a->receive(from, buf));  // non-blocking, nothing there
  CHECK(!a->wait_readable(0));

  const std::vector<std::uint8_t> msg{1, 2, 3, 4, 5};
  CHECK(a->send(b->local_address(), msg));
  auto n = receive_soon(*b, from, buf);
  CHECK(n && *n == 5 && from == a->local_address());
  CHECK(std::equal(msg.begin(), msg.end(), buf.begin()));
  CHECK(b->send(from, msg));  // reply to the address we saw
  CHECK(receive_soon(*a, from, buf).value_or(0) == 5 && from == b->local_address());

  // A full-size datagram passes whole; a bigger one is cut one past the limit,
  // so the receiver can tell it is too long.
  const std::vector<std::uint8_t> full(kMaxDatagram, 7), big(1500, 8);
  CHECK(a->send(b->local_address(), full));
  CHECK(receive_soon(*b, from, buf).value_or(0) == kMaxDatagram);
  CHECK(a->send(b->local_address(), big));
  CHECK(receive_soon(*b, from, buf).value_or(0) == kMaxDatagram + 1);

  // An empty datagram is a datagram.
  CHECK(a->send(b->local_address(), {}));
  CHECK(receive_soon(*b, from, buf).value_or(99) == 0);

  // IPv4 sockets cannot send to IPv6; a closed port does not break the sender.
  CHECK(!a->send(Address::loopback_v6(9), msg));
  Address dead = b->local_address();
  b.reset();
  CHECK(a->send(dead, msg));
  CHECK(!a->receive(from, buf));
}

void v6_exchange() {
  std::string err;
  auto a = UdpSocket::bind(Address::loopback_v6(0), &err);
  auto b = UdpSocket::bind(Address::loopback_v6(0), &err);
  if (!a || !b) {
    std::fprintf(stderr, "skipping IPv6 test: %s\n", err.c_str());
    return;
  }
  std::vector<std::uint8_t> buf(kMaxDatagram + 1);
  Address from;
  CHECK(a->send(b->local_address(), std::vector<std::uint8_t>{9, 9}));
  CHECK(receive_soon(*b, from, buf).value_or(0) == 2 && from == a->local_address());
}

void bind_failure_reports() {
  auto a = UdpSocket::bind(Address::loopback_v4(0));
  std::string err;
  CHECK(!UdpSocket::bind(a->local_address(), &err));  // address in use
  CHECK(!err.empty());
}

}  // namespace

int main() {
  addresses();
  v4_exchange();
  v6_exchange();
  bind_failure_reports();
  return 0;
}
