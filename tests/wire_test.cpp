// Wire format: round trips, bounds, and rejection of malformed datagrams.
#include <vector>

#include "check.hpp"
#include "npk/transport/wire.hpp"

using namespace npk::transport;

namespace {

std::vector<std::uint8_t> bytes_of(const PacketBuilder& b) {
  return {b.bytes().begin(), b.bytes().end()};
}

void header_round_trip() {
  Header h{PacketType::Data, 0xdeadbeef, 0xfffe, 7, 0x80000001};
  PacketBuilder b(h);
  CHECK(b.bytes().size() == kHeaderSize);
  Packet p;
  CHECK(parse_packet(b.bytes(), p) == ParseError::None);
  CHECK(p.header.type == PacketType::Data && p.header.connection == 0xdeadbeef);
  CHECK(p.header.seq == 0xfffe && p.header.ack == 7 && p.header.ack_bits == 0x80000001);
  CHECK(p.body.empty());
}

void header_rejections() {
  Packet p;
  std::vector<std::uint8_t> d(kHeaderSize, 0);
  CHECK(parse_packet({d.data(), 5}, p) == ParseError::TooShort);
  CHECK(parse_packet(d, p) == ParseError::BadMagic);
  auto v = bytes_of(PacketBuilder(Header{}));
  v[2] = 9;
  CHECK(parse_packet(v, p) == ParseError::BadVersion);
  v[2] = kVersion;
  v[3] = 0;
  CHECK(parse_packet(v, p) == ParseError::BadType);
  v[3] = 6;
  CHECK(parse_packet(v, p) == ParseError::BadType);
  v.assign(kMaxDatagram + 1, 0);
  CHECK(parse_packet(v, p) == ParseError::TooLong);
}

void handshake_round_trip() {
  Packet p;
  PacketBuilder init(Header{PacketType::Init});
  CHECK(init.init(0x1122334455667788ULL));
  CHECK(parse_packet(init.bytes(), p) == ParseError::TooShort);  // an unpadded request
  CHECK(init.pad_to(kMinHandshakeRequest));
  CHECK(parse_packet(init.bytes(), p) == ParseError::None);
  std::uint64_t nonce = 0;
  CHECK(parse_init(p.body, nonce) == ParseError::None && nonce == 0x1122334455667788ULL);

  const ChallengeBody cb{0xaa, 0x01020304, 0xfeedfacecafebeefULL};
  PacketBuilder ch(Header{PacketType::Challenge});
  CHECK(ch.challenge(cb));
  CHECK(parse_packet(ch.bytes(), p) == ParseError::None);
  ChallengeBody got;
  CHECK(parse_challenge(p.body, got) == ParseError::None);
  CHECK(got.client_nonce == 0xaa && got.bucket == 0x01020304 && got.cookie == cb.cookie);
  CHECK(parse_challenge({p.body.data(), p.body.size() - 1}, got) == ParseError::Truncated);

  PacketBuilder rs(Header{PacketType::Response});
  CHECK(rs.response(cb));
  CHECK(parse_packet(rs.bytes(), p) == ParseError::TooShort);
  CHECK(rs.pad_to(kMinHandshakeRequest));
  CHECK(parse_packet(rs.bytes(), p) == ParseError::None);
  CHECK(parse_response(p.body, got) == ParseError::None && got.cookie == cb.cookie);

  PacketBuilder ac(Header{PacketType::Accept, 99});
  CHECK(ac.accept({0xaa, 123456789}));
  CHECK(parse_packet(ac.bytes(), p) == ParseError::None && p.header.connection == 99);
  AcceptBody ab;
  CHECK(parse_accept(p.body, ab) == ParseError::None);
  CHECK(ab.client_nonce == 0xaa && ab.server_time_us == 123456789);
}

void frames_round_trip() {
  const std::vector<std::uint8_t> a{1, 2, 3}, c{9, 8, 7, 6, 5};
  PacketBuilder b(Header{PacketType::Data, 5});
  CHECK(b.add_unreliable(a));
  CHECK(b.add_reliable(65535, kFragBegin | kFragEnd, c));
  CHECK(b.add_ping(77, 1000));
  CHECK(b.add_pong(77, 1000, 2000, 2001));
  CHECK(b.add_close(3));
  CHECK(b.add_unreliable({}));
  Packet p;
  CHECK(parse_packet(b.bytes(), p) == ParseError::None);

  FrameReader r(p.body);
  Frame f;
  CHECK(r.next(f) == ParseError::None && f.kind == FrameKind::Unreliable);
  CHECK(std::vector<std::uint8_t>(f.data.begin(), f.data.end()) == a);
  CHECK(r.next(f) == ParseError::None && f.kind == FrameKind::Reliable);
  CHECK(f.reliable_seq == 65535 && f.flags == (kFragBegin | kFragEnd));
  CHECK(std::vector<std::uint8_t>(f.data.begin(), f.data.end()) == c);
  CHECK(r.next(f) == ParseError::None && f.kind == FrameKind::Ping);
  CHECK(f.ping_id == 77 && f.t1 == 1000);
  CHECK(r.next(f) == ParseError::None && f.kind == FrameKind::Pong);
  CHECK(f.t1 == 1000 && f.t2 == 2000 && f.t3 == 2001);
  CHECK(r.next(f) == ParseError::None && f.kind == FrameKind::Close && f.close_reason == 3);
  CHECK(r.next(f) == ParseError::None && f.kind == FrameKind::Unreliable && f.data.empty());
  CHECK(r.done());
}

void frame_rejections() {
  Frame f;
  {  // a length that runs past the datagram
    const std::uint8_t d[] = {1, 0xff, 0xff, 1, 2};
    FrameReader r(d);
    CHECK(r.next(f) == ParseError::Truncated && r.done());
  }
  {  // unknown kind
    const std::uint8_t d[] = {0x7f, 0, 0};
    FrameReader r(d);
    CHECK(r.next(f) == ParseError::BadFrame && r.done());
  }
  {  // unknown reliable flags
    const std::uint8_t d[] = {2, 0, 1, 0x80, 0, 0};
    FrameReader r(d);
    CHECK(r.next(f) == ParseError::BadFrame);
  }
  {  // a cut-off ping
    const std::uint8_t d[] = {3, 0, 0, 0, 1, 0, 0};
    FrameReader r(d);
    CHECK(r.next(f) == ParseError::Truncated);
  }
}

void builder_bounds() {
  PacketBuilder b(Header{});
  const std::vector<std::uint8_t> big(kMaxFragment, 0xab);
  int added = 0;
  while (b.add_unreliable(big)) ++added;
  CHECK(added == 1);  // 1200 - 16 = 1184: one 1024-byte frame (1027), not two
  CHECK(b.bytes().size() <= kMaxDatagram);
  const std::size_t before = b.bytes().size();
  CHECK(!b.add_pong(1, 2, 3, 4) || b.bytes().size() > before);  // unchanged when it fails
  CHECK(!b.pad_to(kMaxDatagram + 1));
  CHECK(b.pad_to(kMaxDatagram) && b.bytes().size() == kMaxDatagram);
  CHECK(!b.add_close(0));
  const std::vector<std::uint8_t> huge(0x10000, 0);
  PacketBuilder c(Header{});
  CHECK(!c.add_unreliable(huge));
}

void sequence_arithmetic() {
  CHECK(seq_newer(1, 0));
  CHECK(!seq_newer(0, 1));
  CHECK(!seq_newer(5, 5));
  CHECK(seq_newer(0, 65535));
  CHECK(!seq_newer(65535, 0));
  CHECK(seq_newer(0x7fff, 0));
  CHECK(!seq_newer(0x8000, 0));
}

}  // namespace

int main() {
  header_round_trip();
  header_rejections();
  handshake_round_trip();
  frames_round_trip();
  frame_rejections();
  builder_bounds();
  sequence_arithmetic();
  return 0;
}
