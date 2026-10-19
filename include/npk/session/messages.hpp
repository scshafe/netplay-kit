#pragma once

// The session layer's messages, carried as the payload of a transport message.
// Big-endian, like the transport. Decoding is for hostile input: every length is
// checked against the message and against a bound the caller gives, nothing
// throws, and a decoded view points into the message it came from.
//
// Unreliable (sent every frame, redundantly, so a lost one costs nothing):
//   Inputs  client -> server  type u8 | first u32 | canonical_ack u32 | count u8 | count * input
//           the client's own inputs for frames first .. first+count-1, and the
//           first frame whose canonical input it still needs
//   Frames  server -> client  type u8 | first u32 | count u8 | count * (players * input)
//           canonical inputs of frames first .. first+count-1
// Reliable (control, rare):
//   Start   server -> client  type u8 | player u8 | players u8 | input_size u8 |
//                             tick_rate u16 | hash_interval u16 | start_time u64
//           frame 0 begins at start_time on the server's clock
//   Hash    client -> server  type u8 | frame u32 | hash u64     (a checkpoint)
//   Resync  server -> client  type u8 | frame u32 | snapshot...  (the server's state at frame)

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "npk/session/simulation.hpp"

namespace npk::session::msg {

enum class Type : std::uint8_t {
  Inputs = 1,
  Frames = 2,
  Start = 16,
  Hash = 17,
  Resync = 18,
};

inline constexpr std::size_t kMaxInputFrames = 32;  // frames in one Inputs message
// The most a Frames message may carry, in bytes of canonical inputs; with its
// 6-byte header it fits one datagram with room to spare.
inline constexpr std::size_t kMaxFramesBytes = 1100;

// Whether a game of this shape fits the protocol: 1..16 players, inputs of 1..255
// bytes, and all players' inputs for a frame within kMaxFramesBytes.
inline bool shape_supported(std::size_t players, std::size_t input_size) noexcept {
  return players >= 1 && players <= 16 && input_size >= 1 && input_size <= 255 &&
         players * input_size <= kMaxFramesBytes;
}

std::optional<Type> type_of(std::span<const std::uint8_t> message) noexcept;

struct Inputs {
  Frame first = 0;
  Frame canonical_ack = 0;
  std::size_t count = 0;
  std::span<const std::uint8_t> data;  // count * input_size
};
void encode_inputs(Frame first, Frame canonical_ack, std::size_t count, std::span<const std::uint8_t> data,
                   std::vector<std::uint8_t>& out);
// False unless the message is exactly an Inputs message with 1..kMaxInputFrames inputs of input_size.
bool decode_inputs(std::span<const std::uint8_t> m, std::size_t input_size, Inputs& out) noexcept;

struct Frames {
  Frame first = 0;
  std::size_t count = 0;
  std::span<const std::uint8_t> data;  // count * frame_bytes
};
void encode_frames(Frame first, std::size_t count, std::span<const std::uint8_t> data, std::vector<std::uint8_t>& out);
// frame_bytes = players * input_size. False unless exactly 1..(kMaxFramesBytes / frame_bytes) frames.
bool decode_frames(std::span<const std::uint8_t> m, std::size_t frame_bytes, Frames& out) noexcept;

struct Start {
  std::uint8_t player = 0, players = 0, input_size = 0;
  std::uint16_t tick_rate = 0;
  std::uint16_t hash_interval = 0;  // frames between checkpoints; 0: none
  std::uint64_t start_time = 0;
};
void encode_start(const Start& s, std::vector<std::uint8_t>& out);
bool decode_start(std::span<const std::uint8_t> m, Start& out) noexcept;

struct Hash {
  Frame frame = 0;
  std::uint64_t hash = 0;
};
void encode_hash(const Hash& h, std::vector<std::uint8_t>& out);
bool decode_hash(std::span<const std::uint8_t> m, Hash& out) noexcept;

struct Resync {
  Frame frame = 0;
  std::span<const std::uint8_t> snapshot;
};
void encode_resync(Frame frame, std::span<const std::uint8_t> snapshot, std::vector<std::uint8_t>& out);
// max_snapshot bounds the snapshot; a longer message is refused.
bool decode_resync(std::span<const std::uint8_t> m, std::size_t max_snapshot, Resync& out) noexcept;

}  // namespace npk::session::msg
