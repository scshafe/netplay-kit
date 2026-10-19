// A fuzz-style test of the packet parser, run by ctest with a fixed seed so a
// failure reproduces. Sanitizers (Debug builds) turn an out-of-bounds read into
// a failure. Properties:
//   - arbitrary bytes never crash the parser, and every parsed view lies inside
//     the datagram;
//   - a frame loop always terminates and consumes the whole body or errors;
//   - valid packets survive mutation and truncation without crashing;
//   - random valid packets round-trip exactly.
#include <random>
#include <vector>

#include "check.hpp"
#include "npk/transport/wire.hpp"

using namespace npk::transport;

namespace {

using Bytes = std::vector<std::uint8_t>;

bool inside(std::span<const std::uint8_t> view, std::span<const std::uint8_t> whole) {
  if (view.empty()) return true;
  return view.data() >= whole.data() && view.data() + view.size() <= whole.data() + whole.size();
}

// Parses everything the parser offers for a datagram and checks the invariants.
void exercise(std::span<const std::uint8_t> d) {
  Packet p;
  const ParseError e = parse_packet(d, p);
  if (e != ParseError::None) return;
  CHECK(d.size() <= kMaxDatagram && d.size() >= kHeaderSize);
  CHECK(inside(p.body, d));
  switch (p.header.type) {
    case PacketType::Init: {
      std::uint64_t n;
      CHECK(d.size() >= kMinHandshakeRequest);
      (void)parse_init(p.body, n);
      break;
    }
    case PacketType::Challenge: {
      ChallengeBody b;
      (void)parse_challenge(p.body, b);
      break;
    }
    case PacketType::Response: {
      ChallengeBody b;
      CHECK(d.size() >= kMinHandshakeRequest);
      (void)parse_response(p.body, b);
      break;
    }
    case PacketType::Accept: {
      AcceptBody b;
      (void)parse_accept(p.body, b);
      break;
    }
    case PacketType::Data: {
      FrameReader r(p.body);
      std::size_t steps = 0;
      Frame f;
      while (!r.done()) {
        CHECK(++steps <= p.body.size());  // every frame is at least one byte
        if (r.next(f) != ParseError::None) {
          CHECK(r.done());
          break;
        }
        CHECK(inside(f.data, d));
      }
      break;
    }
  }
}

Bytes random_valid(std::mt19937_64& rng) {
  const auto pick = [&](std::uint64_t n) { return rng() % n; };
  Header h;
  h.type = static_cast<PacketType>(1 + pick(5));
  h.connection = static_cast<std::uint32_t>(rng());
  h.seq = static_cast<std::uint16_t>(rng());
  h.ack = static_cast<std::uint16_t>(rng());
  h.ack_bits = static_cast<std::uint32_t>(rng());
  PacketBuilder b(h);
  const ChallengeBody cb{rng(), static_cast<std::uint32_t>(rng()), rng()};
  switch (h.type) {
    case PacketType::Init:
      b.init(rng());
      b.pad_to(kMinHandshakeRequest + pick(kMaxDatagram - kMinHandshakeRequest + 1));
      break;
    case PacketType::Challenge: b.challenge(cb); break;
    case PacketType::Response:
      b.response(cb);
      b.pad_to(kMinHandshakeRequest + pick(kMaxDatagram - kMinHandshakeRequest + 1));
      break;
    case PacketType::Accept: b.accept({rng(), rng()}); break;
    case PacketType::Data: {
      const int n = static_cast<int>(pick(8));
      for (int i = 0; i < n; ++i) {
        Bytes payload(pick(300));
        for (auto& x : payload) x = static_cast<std::uint8_t>(rng());
        switch (pick(5)) {
          case 0: b.add_unreliable(payload); break;
          case 1: b.add_reliable(static_cast<std::uint16_t>(rng()),
                                 static_cast<std::uint8_t>(pick(4)), payload); break;
          case 2: b.add_ping(static_cast<std::uint32_t>(rng()), rng()); break;
          case 3: b.add_pong(static_cast<std::uint32_t>(rng()), rng(), rng(), rng()); break;
          default: b.add_close(static_cast<std::uint8_t>(rng())); break;
        }
      }
      break;
    }
  }
  return {b.bytes().begin(), b.bytes().end()};
}

}  // namespace

int main() {
  std::mt19937_64 rng(0x6e706b21);

  // Arbitrary bytes, biased towards a plausible prefix so the deeper code runs.
  for (int i = 0; i < 200000; ++i) {
    Bytes d(rng() % (kMaxDatagram + 64));
    for (auto& x : d) x = static_cast<std::uint8_t>(rng());
    if (d.size() >= 4 && (i & 1)) {
      d[0] = kMagic >> 8;
      d[1] = kMagic & 0xff;
      d[2] = kVersion;
      d[3] = static_cast<std::uint8_t>(1 + rng() % 5);
    }
    exercise(d);
  }

  // Valid packets: round trip, then every truncation, then random mutations.
  for (int i = 0; i < 5000; ++i) {
    const Bytes d = random_valid(rng);
    Packet p;
    CHECK(parse_packet(d, p) == ParseError::None);
    if (p.header.type == PacketType::Data) {
      FrameReader r(p.body);
      Frame f;
      while (!r.done()) CHECK(r.next(f) == ParseError::None);
    }
    for (std::size_t n = 0; n <= d.size(); ++n) exercise({d.data(), n});
    for (int m = 0; m < 20; ++m) {
      Bytes x = d;
      const int flips = 1 + static_cast<int>(rng() % 4);
      for (int k = 0; k < flips; ++k) x[rng() % x.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
      exercise(x);
    }
  }
  return 0;
}
