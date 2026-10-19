#include "npk/transport/wire.hpp"

#include <algorithm>
#include <cstring>

namespace npk::transport {
namespace {

// A cursor over a body. Reads fail (and stay failed) rather than run past the end.
class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> s) noexcept : s_(s) {}
  bool ok() const noexcept { return ok_; }
  std::size_t left() const noexcept { return s_.size(); }

  std::uint64_t uint(std::size_t n) noexcept {
    if (!ok_ || s_.size() < n) {
      ok_ = false;
      return 0;
    }
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) v = (v << 8) | s_[i];
    s_ = s_.subspan(n);
    return v;
  }
  std::uint8_t u8() noexcept { return static_cast<std::uint8_t>(uint(1)); }
  std::uint16_t u16() noexcept { return static_cast<std::uint16_t>(uint(2)); }
  std::uint32_t u32() noexcept { return static_cast<std::uint32_t>(uint(4)); }
  std::uint64_t u64() noexcept { return uint(8); }

  std::span<const std::uint8_t> bytes(std::size_t n) noexcept {
    if (!ok_ || s_.size() < n) {
      ok_ = false;
      return {};
    }
    auto out = s_.first(n);
    s_ = s_.subspan(n);
    return out;
  }
  std::span<const std::uint8_t> rest() const noexcept { return s_; }

 private:
  std::span<const std::uint8_t> s_;
  bool ok_ = true;
};

void be(std::array<std::uint8_t, 8>& tmp, std::uint64_t v, std::size_t n) noexcept {
  for (std::size_t i = 0; i < n; ++i) tmp[i] = static_cast<std::uint8_t>(v >> (8 * (n - 1 - i)));
}

bool known_type(std::uint8_t t) noexcept { return t >= 1 && t <= 5; }

ParseError parse_challenge_fields(std::span<const std::uint8_t> body, ChallengeBody& out) noexcept {
  Reader r(body);
  ChallengeBody b;
  b.client_nonce = r.u64();
  b.bucket = r.u32();
  b.cookie = r.u64();
  if (!r.ok()) return ParseError::Truncated;
  out = b;
  return ParseError::None;
}

}  // namespace

ParseError parse_packet(std::span<const std::uint8_t> d, Packet& out) noexcept {
  if (d.size() > kMaxDatagram) return ParseError::TooLong;
  if (d.size() < kHeaderSize) return ParseError::TooShort;
  Reader r(d);
  if (r.u16() != kMagic) return ParseError::BadMagic;
  if (r.u8() != kVersion) return ParseError::BadVersion;
  const std::uint8_t type = r.u8();
  if (!known_type(type)) return ParseError::BadType;
  Packet p;
  p.header.type = static_cast<PacketType>(type);
  p.header.connection = r.u32();
  p.header.seq = r.u16();
  p.header.ack = r.u16();
  p.header.ack_bits = r.u32();
  p.body = r.rest();
  if ((p.header.type == PacketType::Init || p.header.type == PacketType::Response) &&
      d.size() < kMinHandshakeRequest) {
    return ParseError::TooShort;
  }
  out = p;
  return ParseError::None;
}

ParseError parse_init(std::span<const std::uint8_t> body, std::uint64_t& client_nonce) noexcept {
  Reader r(body);
  const std::uint64_t n = r.u64();
  if (!r.ok()) return ParseError::Truncated;
  client_nonce = n;
  return ParseError::None;
}

ParseError parse_challenge(std::span<const std::uint8_t> body, ChallengeBody& out) noexcept {
  return parse_challenge_fields(body, out);
}

ParseError parse_response(std::span<const std::uint8_t> body, ChallengeBody& out) noexcept {
  return parse_challenge_fields(body, out);
}

ParseError parse_accept(std::span<const std::uint8_t> body, AcceptBody& out) noexcept {
  Reader r(body);
  AcceptBody b;
  b.client_nonce = r.u64();
  b.server_time_us = r.u64();
  if (!r.ok()) return ParseError::Truncated;
  out = b;
  return ParseError::None;
}

ParseError FrameReader::next(Frame& out) noexcept {
  Reader r(rest_);
  Frame f;
  const std::uint8_t kind = r.u8();
  ParseError err = ParseError::None;
  switch (kind) {
    case static_cast<std::uint8_t>(FrameKind::Unreliable): {
      f.kind = FrameKind::Unreliable;
      const std::size_t len = r.u16();
      f.data = r.bytes(len);
      break;
    }
    case static_cast<std::uint8_t>(FrameKind::Reliable): {
      f.kind = FrameKind::Reliable;
      f.reliable_seq = r.u16();
      f.flags = r.u8();
      const std::size_t len = r.u16();
      f.data = r.bytes(len);
      if (r.ok() && (f.flags & ~(kFragBegin | kFragEnd)) != 0) err = ParseError::BadFrame;
      break;
    }
    case static_cast<std::uint8_t>(FrameKind::Ping):
      f.kind = FrameKind::Ping;
      f.ping_id = r.u32();
      f.t1 = r.u64();
      break;
    case static_cast<std::uint8_t>(FrameKind::Pong):
      f.kind = FrameKind::Pong;
      f.ping_id = r.u32();
      f.t1 = r.u64();
      f.t2 = r.u64();
      f.t3 = r.u64();
      break;
    case static_cast<std::uint8_t>(FrameKind::Close):
      f.kind = FrameKind::Close;
      f.close_reason = r.u8();
      break;
    default:
      err = ParseError::BadFrame;
      break;
  }
  if (err == ParseError::None && !r.ok()) err = ParseError::Truncated;
  if (err != ParseError::None) {
    rest_ = {};
    return err;
  }
  rest_ = r.rest();
  out = f;
  return ParseError::None;
}

PacketBuilder::PacketBuilder(const Header& h) noexcept {
  std::array<std::uint8_t, 8> t{};
  auto put_be = [&](std::uint64_t v, std::size_t n) {
    be(t, v, n);
    put({t.data(), n});
  };
  put_be(kMagic, 2);
  put_be(kVersion, 1);
  put_be(static_cast<std::uint8_t>(h.type), 1);
  put_be(h.connection, 4);
  put_be(h.seq, 2);
  put_be(h.ack, 2);
  put_be(h.ack_bits, 4);
}

bool PacketBuilder::put(std::span<const std::uint8_t> head,
                        std::span<const std::uint8_t> tail) noexcept {
  if (head.size() + tail.size() > remaining()) return false;
  for (auto part : {head, tail}) {
    if (!part.empty()) std::memcpy(buf_.data() + size_, part.data(), part.size());
    size_ += part.size();
  }
  return true;
}

namespace {

// The fixed-size fields of one frame or body, encoded before the append.
struct Fields {
  std::array<std::uint8_t, 40> buf{};
  std::size_t n = 0;
  std::span<const std::uint8_t> view() const noexcept { return {buf.data(), n}; }
  void u(std::uint64_t v, std::size_t bytes) noexcept {
    std::array<std::uint8_t, 8> t{};
    be(t, v, bytes);
    std::memcpy(buf.data() + n, t.data(), bytes);
    n += bytes;
  }
};

}  // namespace

bool PacketBuilder::init(std::uint64_t client_nonce) noexcept {
  Fields f;
  f.u(client_nonce, 8);
  return put(f.view());
}

bool PacketBuilder::challenge(const ChallengeBody& b) noexcept {
  Fields f;
  f.u(b.client_nonce, 8);
  f.u(b.bucket, 4);
  f.u(b.cookie, 8);
  return put(f.view());
}

bool PacketBuilder::response(const ChallengeBody& b) noexcept { return challenge(b); }

bool PacketBuilder::accept(const AcceptBody& b) noexcept {
  Fields f;
  f.u(b.client_nonce, 8);
  f.u(b.server_time_us, 8);
  return put(f.view());
}

bool PacketBuilder::add_unreliable(std::span<const std::uint8_t> data) noexcept {
  if (data.size() > 0xffff) return false;
  Fields f;
  f.u(static_cast<std::uint8_t>(FrameKind::Unreliable), 1);
  f.u(data.size(), 2);
  return put(f.view(), data);
}

bool PacketBuilder::add_reliable(std::uint16_t seq, std::uint8_t flags,
                                 std::span<const std::uint8_t> data) noexcept {
  if (data.size() > 0xffff) return false;
  Fields f;
  f.u(static_cast<std::uint8_t>(FrameKind::Reliable), 1);
  f.u(seq, 2);
  f.u(flags, 1);
  f.u(data.size(), 2);
  return put(f.view(), data);
}

bool PacketBuilder::add_ping(std::uint32_t id, std::uint64_t t1) noexcept {
  Fields f;
  f.u(static_cast<std::uint8_t>(FrameKind::Ping), 1);
  f.u(id, 4);
  f.u(t1, 8);
  return put(f.view());
}

bool PacketBuilder::add_pong(std::uint32_t id, std::uint64_t t1, std::uint64_t t2,
                             std::uint64_t t3) noexcept {
  Fields f;
  f.u(static_cast<std::uint8_t>(FrameKind::Pong), 1);
  f.u(id, 4);
  f.u(t1, 8);
  f.u(t2, 8);
  f.u(t3, 8);
  return put(f.view());
}

bool PacketBuilder::add_close(std::uint8_t reason) noexcept {
  Fields f;
  f.u(static_cast<std::uint8_t>(FrameKind::Close), 1);
  f.u(reason, 1);
  return put(f.view());
}

bool PacketBuilder::pad_to(std::size_t size) noexcept {
  if (size > kMaxDatagram) return false;
  if (size > size_) {
    std::fill(buf_.begin() + static_cast<std::ptrdiff_t>(size_),
              buf_.begin() + static_cast<std::ptrdiff_t>(size), std::uint8_t{0});
    size_ = size;
  }
  return true;
}

}  // namespace npk::transport
