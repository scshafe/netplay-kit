#pragma once

// The wire format. Every multi-byte integer is big-endian. A datagram is at most
// kMaxDatagram bytes of UDP payload (docs/design.md, "Packet size"), starts with
// a fixed header and carries a type-specific body:
//
//   header (16): magic u16 | version u8 | type u8 | connection u32 | seq u16 |
//                ack u16 | ack_bits u32
//   Init       : client_nonce u64, padding           (datagram >= kMinHandshakeRequest)
//   Challenge  : client_nonce u64, bucket u32, cookie u64
//   Response   : client_nonce u64, bucket u32, cookie u64, padding
//                                                    (datagram >= kMinHandshakeRequest)
//   Accept     : client_nonce u64, server_time_us u64
//   Data       : frames until the end of the datagram (ack-only when empty)
//
// A frame starts with a kind byte:
//   Unreliable : len u16, bytes
//   Reliable   : seq u16, flags u8 (kFragBegin | kFragEnd), len u16, bytes
//   Ping       : id u32, t1 u64
//   Pong       : id u32, t1 u64, t2 u64, t3 u64
//   Close      : reason u8
//
// Parsing is for hostile input: nothing here throws, allocates from a length the
// sender chose or reads past the datagram. Parsed byte ranges are views into it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace npk::transport {

inline constexpr std::size_t kMaxDatagram = 1200;
inline constexpr std::size_t kHeaderSize = 16;
// Handshake requests are padded to this size so no reply can be larger.
inline constexpr std::size_t kMinHandshakeRequest = 128;
inline constexpr std::uint16_t kMagic = 0x4e50;  // "NP"
inline constexpr std::uint8_t kVersion = 1;
// Largest piece of a reliable message sent in one frame.
inline constexpr std::size_t kMaxFragment = 1024;

enum class PacketType : std::uint8_t {
  Init = 1,
  Challenge = 2,
  Response = 3,
  Accept = 4,
  Data = 5,
};

struct Header {
  PacketType type = PacketType::Data;
  std::uint32_t connection = 0;  // 0 until the handshake assigns one
  std::uint16_t seq = 0;         // this packet's sequence number
  std::uint16_t ack = 0;         // newest packet sequence received from the peer
  std::uint32_t ack_bits = 0;    // bit i set: packet (ack - 1 - i) was received
};

enum class ParseError : std::uint8_t {
  None = 0,
  TooShort,   // smaller than the header, or than the type requires
  TooLong,    // larger than kMaxDatagram
  BadMagic,
  BadVersion,
  BadType,
  Truncated,  // a body or frame runs past the end of the datagram
  BadFrame,   // unknown frame kind or flags
};

struct Packet {
  Header header;
  std::span<const std::uint8_t> body;  // after the header
};

// Checks size, magic, version and type, and the minimum size for the type.
ParseError parse_packet(std::span<const std::uint8_t> datagram, Packet& out) noexcept;

struct ChallengeBody {
  std::uint64_t client_nonce = 0;
  std::uint32_t bucket = 0;
  std::uint64_t cookie = 0;
};
struct AcceptBody {
  std::uint64_t client_nonce = 0;
  std::uint64_t server_time_us = 0;
};

ParseError parse_init(std::span<const std::uint8_t> body, std::uint64_t& client_nonce) noexcept;
ParseError parse_challenge(std::span<const std::uint8_t> body, ChallengeBody& out) noexcept;
// A Response carries the same fields as a Challenge, echoed.
ParseError parse_response(std::span<const std::uint8_t> body, ChallengeBody& out) noexcept;
ParseError parse_accept(std::span<const std::uint8_t> body, AcceptBody& out) noexcept;

enum class FrameKind : std::uint8_t {
  Unreliable = 1,
  Reliable = 2,
  Ping = 3,
  Pong = 4,
  Close = 5,
};

inline constexpr std::uint8_t kFragBegin = 0x01;
inline constexpr std::uint8_t kFragEnd = 0x02;

struct Frame {
  FrameKind kind = FrameKind::Unreliable;
  std::span<const std::uint8_t> data;  // Unreliable, Reliable
  std::uint16_t reliable_seq = 0;      // Reliable
  std::uint8_t flags = 0;              // Reliable
  std::uint32_t ping_id = 0;           // Ping, Pong
  std::uint64_t t1 = 0;                // Ping, Pong: the pinger's clock when it sent
  std::uint64_t t2 = 0;                // Pong: the responder's clock when it received
  std::uint64_t t3 = 0;                // Pong: the responder's clock when it sent
  std::uint8_t close_reason = 0;       // Close
};

// Walks the frames of a Data body. Each next() consumes at least one byte, so a
// loop `while (!r.done())` ends. After an error the reader is done: the rest of
// the datagram is not trusted.
class FrameReader {
 public:
  explicit FrameReader(std::span<const std::uint8_t> body) noexcept : rest_(body) {}
  bool done() const noexcept { return rest_.empty(); }
  ParseError next(Frame& out) noexcept;

 private:
  std::span<const std::uint8_t> rest_;
};

// Builds one datagram, never past kMaxDatagram. Each append returns false and
// leaves the datagram unchanged when it does not fit.
class PacketBuilder {
 public:
  explicit PacketBuilder(const Header& header) noexcept;

  bool init(std::uint64_t client_nonce) noexcept;  // then pad_to
  bool challenge(const ChallengeBody& b) noexcept;
  bool response(const ChallengeBody& b) noexcept;  // then pad_to
  bool accept(const AcceptBody& b) noexcept;

  bool add_unreliable(std::span<const std::uint8_t> data) noexcept;
  bool add_reliable(std::uint16_t seq, std::uint8_t flags,
                    std::span<const std::uint8_t> data) noexcept;
  bool add_ping(std::uint32_t id, std::uint64_t t1) noexcept;
  bool add_pong(std::uint32_t id, std::uint64_t t1, std::uint64_t t2, std::uint64_t t3) noexcept;
  bool add_close(std::uint8_t reason) noexcept;

  // Zero-fills up to size (no-op when already that large). False if size > kMaxDatagram.
  bool pad_to(std::size_t size) noexcept;

  std::size_t remaining() const noexcept { return kMaxDatagram - size_; }
  std::span<const std::uint8_t> bytes() const noexcept { return {buf_.data(), size_}; }

 private:
  // Appends head then tail, or nothing when both do not fit.
  bool put(std::span<const std::uint8_t> head, std::span<const std::uint8_t> tail = {}) noexcept;
  std::array<std::uint8_t, kMaxDatagram> buf_{};
  std::size_t size_ = 0;
};

// Serial-number arithmetic on 16-bit sequence numbers: true when a is newer than b.
constexpr bool seq_newer(std::uint16_t a, std::uint16_t b) noexcept {
  return a != b && static_cast<std::uint16_t>(a - b) < 0x8000;
}

}  // namespace npk::transport
