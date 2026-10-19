#include "npk/session/messages.hpp"

#include <algorithm>

namespace npk::session::msg {

namespace {

void put32(std::vector<std::uint8_t>& o, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) o.push_back(static_cast<std::uint8_t>(v >> s));
}
void put64(std::vector<std::uint8_t>& o, std::uint64_t v) {
  for (int s = 56; s >= 0; s -= 8) o.push_back(static_cast<std::uint8_t>(v >> s));
}
std::uint32_t get32(const std::uint8_t* p) {
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
std::uint64_t get64(const std::uint8_t* p) { return (std::uint64_t{get32(p)} << 32) | get32(p + 4); }

bool is(std::span<const std::uint8_t> m, Type t) { return !m.empty() && m[0] == static_cast<std::uint8_t>(t); }

}  // namespace

std::optional<Type> type_of(std::span<const std::uint8_t> m) noexcept {
  if (m.empty()) return std::nullopt;
  switch (static_cast<Type>(m[0])) {
    case Type::Inputs:
    case Type::Frames:
    case Type::Start:
    case Type::Hash:
    case Type::Resync:
      return static_cast<Type>(m[0]);
  }
  return std::nullopt;
}

void encode_inputs(Frame first, Frame ack, std::size_t count, std::span<const std::uint8_t> data,
                   std::vector<std::uint8_t>& out) {
  out.clear();
  out.push_back(static_cast<std::uint8_t>(Type::Inputs));
  put32(out, first);
  put32(out, ack);
  out.push_back(static_cast<std::uint8_t>(count));
  out.insert(out.end(), data.begin(), data.end());
}

bool decode_inputs(std::span<const std::uint8_t> m, std::size_t input_size, Inputs& out) noexcept {
  constexpr std::size_t kHead = 1 + 4 + 4 + 1;
  if (!is(m, Type::Inputs) || m.size() < kHead || input_size == 0) return false;
  const std::size_t count = m[9];
  if (count == 0 || count > kMaxInputFrames || m.size() != kHead + count * input_size) return false;
  out.first = get32(&m[1]);
  out.canonical_ack = get32(&m[5]);
  out.count = count;
  out.data = m.subspan(kHead);
  return true;
}

void encode_frames(Frame first, std::size_t count, std::span<const std::uint8_t> data,
                   std::vector<std::uint8_t>& out) {
  out.clear();
  out.push_back(static_cast<std::uint8_t>(Type::Frames));
  put32(out, first);
  out.push_back(static_cast<std::uint8_t>(count));
  out.insert(out.end(), data.begin(), data.end());
}

bool decode_frames(std::span<const std::uint8_t> m, std::size_t frame_bytes, Frames& out) noexcept {
  constexpr std::size_t kHead = 1 + 4 + 1;
  if (!is(m, Type::Frames) || m.size() < kHead || frame_bytes == 0) return false;
  const std::size_t count = m[5];
  if (count == 0 || count * frame_bytes > kMaxFramesBytes || m.size() != kHead + count * frame_bytes) return false;
  out.first = get32(&m[1]);
  out.count = count;
  out.data = m.subspan(kHead);
  return true;
}

void encode_start(const Start& s, std::vector<std::uint8_t>& out) {
  out.clear();
  out.push_back(static_cast<std::uint8_t>(Type::Start));
  out.push_back(s.player);
  out.push_back(s.players);
  out.push_back(s.input_size);
  out.push_back(static_cast<std::uint8_t>(s.tick_rate >> 8));
  out.push_back(static_cast<std::uint8_t>(s.tick_rate));
  put64(out, s.start_time);
}

bool decode_start(std::span<const std::uint8_t> m, Start& out) noexcept {
  if (!is(m, Type::Start) || m.size() != 1 + 3 + 2 + 8) return false;
  out.player = m[1];
  out.players = m[2];
  out.input_size = m[3];
  out.tick_rate = static_cast<std::uint16_t>((m[4] << 8) | m[5]);
  out.start_time = get64(&m[6]);
  return true;
}

void encode_hash(const Hash& h, std::vector<std::uint8_t>& out) {
  out.clear();
  out.push_back(static_cast<std::uint8_t>(Type::Hash));
  put32(out, h.frame);
  put64(out, h.hash);
}

bool decode_hash(std::span<const std::uint8_t> m, Hash& out) noexcept {
  if (!is(m, Type::Hash) || m.size() != 1 + 4 + 8) return false;
  out.frame = get32(&m[1]);
  out.hash = get64(&m[5]);
  return true;
}

void encode_resync(Frame frame, std::span<const std::uint8_t> snapshot, std::vector<std::uint8_t>& out) {
  out.clear();
  out.push_back(static_cast<std::uint8_t>(Type::Resync));
  put32(out, frame);
  out.insert(out.end(), snapshot.begin(), snapshot.end());
}

bool decode_resync(std::span<const std::uint8_t> m, std::size_t max_snapshot, Resync& out) noexcept {
  if (!is(m, Type::Resync) || m.size() < 5 || m.size() - 5 > max_snapshot) return false;
  out.frame = get32(&m[1]);
  out.snapshot = m.subspan(5);
  return true;
}

}  // namespace npk::session::msg
