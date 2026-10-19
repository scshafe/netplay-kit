// The session messages: round trips, the bounds on every length, and decoding
// garbage (random bytes, and valid messages cut or changed at every position).

#include <cstdio>
#include <random>

#include "check.hpp"
#include "npk/session/messages.hpp"

using namespace npk::session;

namespace {

std::vector<std::uint8_t> bytes(std::size_t n, std::uint8_t v = 7) { return std::vector<std::uint8_t>(n, v); }

void round_trips() {
  std::vector<std::uint8_t> m;
  {
    auto data = bytes(3 * 2);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i);
    msg::encode_inputs(0x01020304, 0x0a0b0c0d, 3, data, m);
    CHECK(msg::type_of(m) == msg::Type::Inputs);
    msg::Inputs in;
    CHECK(msg::decode_inputs(m, 2, in));
    CHECK(in.first == 0x01020304 && in.canonical_ack == 0x0a0b0c0d && in.count == 3);
    CHECK(std::equal(in.data.begin(), in.data.end(), data.begin(), data.end()));
    CHECK(m[1] == 1 && m[4] == 4);  // big-endian
  }
  {
    auto data = bytes(5 * 4, 9);
    msg::encode_frames(77, 5, data, m);
    msg::Frames f;
    CHECK(msg::decode_frames(m, 4, f));
    CHECK(f.first == 77 && f.count == 5 && f.data.size() == 20);
  }
  {
    msg::Start s{.player = 2, .players = 4, .input_size = 2, .tick_rate = 60, .start_time = 0x1122334455667788ULL};
    msg::encode_start(s, m);
    msg::Start d;
    CHECK(msg::decode_start(m, d));
    CHECK(d.player == 2 && d.players == 4 && d.input_size == 2 && d.tick_rate == 60 && d.start_time == s.start_time);
  }
  {
    msg::encode_hash({.frame = 90, .hash = 0xfeedfacecafebeefULL}, m);
    msg::Hash h;
    CHECK(msg::decode_hash(m, h));
    CHECK(h.frame == 90 && h.hash == 0xfeedfacecafebeefULL);
  }
  {
    auto snap = bytes(100, 3);
    msg::encode_resync(120, snap, m);
    msg::Resync r;
    CHECK(msg::decode_resync(m, 100, r));
    CHECK(r.frame == 120 && r.snapshot.size() == 100);
    CHECK(!msg::decode_resync(m, 99, r));  // over the caller's bound
  }
}

void bounds() {
  std::vector<std::uint8_t> m;
  msg::Inputs in;
  msg::encode_inputs(0, 0, 0, {}, m);
  CHECK(!msg::decode_inputs(m, 2, in));  // no inputs
  auto many = bytes((msg::kMaxInputFrames + 1) * 2);
  msg::encode_inputs(0, 0, msg::kMaxInputFrames + 1, many, m);
  CHECK(!msg::decode_inputs(m, 2, in));  // too many
  msg::encode_inputs(0, 0, msg::kMaxInputFrames, std::span(many).first(msg::kMaxInputFrames * 2), m);
  CHECK(msg::decode_inputs(m, 2, in));
  msg::encode_inputs(0, 0, 2, bytes(3), m);
  CHECK(!msg::decode_inputs(m, 2, in));  // count says 2, body has 3 bytes
  msg::encode_inputs(0, 0, 2, bytes(5), m);
  CHECK(!msg::decode_inputs(m, 2, in));  // trailing byte
  CHECK(!msg::decode_inputs(m, 0, in));  // no such game

  msg::Frames f;
  msg::encode_frames(0, 200, bytes(200 * 8), m);
  CHECK(!msg::decode_frames(m, 8, f));  // 1600 bytes of inputs: over kMaxFramesBytes
  msg::encode_frames(0, 137, bytes(137 * 8), m);
  CHECK(msg::decode_frames(m, 8, f));   // 1096
  CHECK(m.size() <= 1100 + 6);
  msg::encode_frames(0, 0, {}, m);
  CHECK(!msg::decode_frames(m, 8, f));

  msg::Start s;
  msg::encode_start({}, m);
  m.push_back(0);
  CHECK(!msg::decode_start(m, s));
  msg::Hash h;
  CHECK(!msg::decode_hash(bytes(13), h));  // wrong type byte
  CHECK(!msg::type_of({}));
  CHECK(!msg::type_of(bytes(1, 99)));

  CHECK(msg::shape_supported(2, 2));
  CHECK(msg::shape_supported(16, 68));
  CHECK(!msg::shape_supported(0, 2));
  CHECK(!msg::shape_supported(17, 2));
  CHECK(!msg::shape_supported(2, 0));
  CHECK(!msg::shape_supported(2, 256));
  CHECK(!msg::shape_supported(16, 69));  // 1104 bytes a frame
}

// Whatever the bytes, a decode either fails or gives views inside the message.
template <class Fn>
void decode_all(std::span<const std::uint8_t> m, Fn&& inside) {
  msg::Inputs a;
  if (msg::decode_inputs(m, 2, a)) inside(a.data, a.count * 2);
  msg::Frames b;
  if (msg::decode_frames(m, 8, b)) inside(b.data, b.count * 8);
  msg::Start c;
  (void)msg::decode_start(m, c);
  msg::Hash d;
  (void)msg::decode_hash(m, d);
  msg::Resync e;
  if (msg::decode_resync(m, 4096, e)) inside(e.snapshot, e.snapshot.size());
  (void)msg::type_of(m);
}

void garbage() {
  std::mt19937_64 rng(42);
  auto inside = [](std::span<const std::uint8_t> v, std::size_t expected) { CHECK(v.size() == expected); };
  for (int i = 0; i < 200000; ++i) {
    std::vector<std::uint8_t> m(rng() % 200);
    for (auto& b : m) b = static_cast<std::uint8_t>(rng());
    if (!m.empty() && i % 2) m[0] = static_cast<std::uint8_t>(1 + rng() % 2 + (rng() % 3 == 0 ? 15 : 0));  // a real type byte
    decode_all(m, inside);
  }
  // Valid messages, cut at every length and with every single byte changed.
  std::vector<std::vector<std::uint8_t>> valid(5);
  msg::encode_inputs(5, 6, 4, bytes(8), valid[0]);
  msg::encode_frames(5, 3, bytes(24), valid[1]);
  msg::encode_start({.player = 1, .players = 2, .input_size = 2, .tick_rate = 60, .start_time = 5}, valid[2]);
  msg::encode_hash({.frame = 30, .hash = 5}, valid[3]);
  msg::encode_resync(30, bytes(50), valid[4]);
  for (const auto& v : valid) {
    for (std::size_t cut = 0; cut <= v.size(); ++cut) decode_all(std::span(v).first(cut), inside);
    for (std::size_t at = 0; at < v.size(); ++at) {
      for (int x = 0; x < 256; ++x) {
        auto w = v;
        w[at] = static_cast<std::uint8_t>(x);
        decode_all(w, inside);
      }
    }
  }
}

}  // namespace

int main() {
  round_trips();
  bounds();
  garbage();
  std::puts("messages_test: ok");
  return 0;
}
