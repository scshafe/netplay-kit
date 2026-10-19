#pragma once

// A small deterministic game for testing the session layer: players that run,
// jump and bump into each other, with a random number generator in the state so
// that any difference in inputs or order soon shows in the hash. Integer only,
// bounded, so it is the same everywhere and sanitizers have nothing to trap.
//
// An input is two bytes: direction (0 left, 1 none, 2 right) and jump (0 or 1).

#include <array>
#include <cstring>

#include "npk/session/simulation.hpp"

namespace {

using npk::session::Frame;

class ToyGame final : public npk::session::Simulation {
 public:
  static constexpr std::size_t kMaxPlayers = 4;
  explicit ToyGame(std::size_t players = 2) : n_(players) {
    for (std::size_t i = 0; i < n_; ++i) p_[i].x = static_cast<std::int32_t>(i) * 3000 + 500;
  }

  std::size_t player_count() const override { return n_; }
  std::size_t input_size() const override { return 2; }

  void step(Frame frame, std::span<const std::uint8_t> in) override {
    for (std::size_t i = 0; i < n_; ++i) {
      P& p = p_[i];
      p.vx = clamp(p.vx + (in[i * 2] - 1) * 3, -60, 60);
      if (in[i * 2 + 1] && p.cooldown == 0 && p.y == 0) {
        p.vy = 80;
        p.cooldown = 20;
      }
      if (p.cooldown) --p.cooldown;
      p.vy = p.y > 0 || p.vy > 0 ? p.vy - 4 : 0;
      p.x = clamp(p.x + p.vx, 0, 10000);
      if (p.x == 0 || p.x == 10000) p.vx = -p.vx / 2;
      p.y = clamp(p.y + p.vy, 0, 5000);
      if (p.y == 0 && p.vy < 0) p.vy = 0;
      p.vx = p.vx * 15 / 16;
    }
    for (std::size_t i = 0; i < n_; ++i) {
      for (std::size_t j = i + 1; j < n_; ++j) {
        if (std::abs(p_[i].x - p_[j].x) < 200 && std::abs(p_[i].y - p_[j].y) < 200) {
          std::swap(p_[i].vx, p_[j].vx);
          p_[i].hp -= static_cast<std::int32_t>(next() % 3);
          p_[j].hp -= static_cast<std::int32_t>(next() % 3);
        }
      }
    }
    rng_ = rng_ * 6364136223846793005ULL + 1442695040888963407ULL + frame;
    if (frame == bug_at_frame || (bug_every && frame % bug_every == 0 && frame > 0)) ++rng_;  // a "non-determinism" bug, for the desync tests
  }

  void snapshot(std::vector<std::uint8_t>& out) const override {
    out.resize(8 + n_ * sizeof(P));
    std::memcpy(out.data(), &rng_, 8);
    std::memcpy(out.data() + 8, p_.data(), n_ * sizeof(P));
  }
  bool restore(std::span<const std::uint8_t> s) override {
    if (s.size() != 8 + n_ * sizeof(P)) return false;
    std::memcpy(&rng_, s.data(), 8);
    std::memcpy(p_.data(), s.data() + 8, n_ * sizeof(P));
    return true;
  }
  std::uint64_t hash() const override {
    std::vector<std::uint8_t> s;
    snapshot(s);
    std::uint64_t h = 1469598103934665603ULL;
    for (auto b : s) h = (h ^ b) * 1099511628211ULL;
    return h;
  }
  bool valid_input(std::span<const std::uint8_t> in) const override { return in.size() == 2 && in[0] < 3 && in[1] < 2; }
  void neutral_input(std::span<std::uint8_t> out) const override {
    for (std::size_t i = 0; i < out.size(); i += 2) {
      out[i] = 1;
      out[i + 1] = 0;
    }
  }

  // Nonzero: step() at this frame changes the state in a way no input explains.
  Frame bug_at_frame = 0xffffffffu;
  Frame bug_every = 0;  // nonzero: and at every multiple of this

 private:
  struct P {
    std::int32_t x = 0, y = 0, vx = 0, vy = 0, hp = 1000;
    std::uint32_t cooldown = 0;
  };
  static std::int32_t clamp(std::int32_t v, std::int32_t lo, std::int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
  std::uint64_t next() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 7;
    rng_ ^= rng_ << 17;
    return rng_;
  }
  std::size_t n_;
  std::array<P, kMaxPlayers> p_{};
  std::uint64_t rng_ = 88172645463325252ULL;
};

// What each player does, as a function of the frame: mostly holds still or keeps
// pressing, sometimes changes. Repeatable from the seed.
inline std::vector<std::uint8_t> toy_inputs(std::uint64_t seed, std::size_t players, Frame frames) {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(frames) * players * 2);
  std::uint64_t s = seed * 0x9e3779b97f4a7c15ULL + 1;
  auto rnd = [&] {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  };
  std::vector<std::uint8_t> cur(players * 2);
  for (std::size_t p = 0; p < players; ++p) { cur[p * 2] = 1; cur[p * 2 + 1] = 0; }
  for (Frame f = 0; f < frames; ++f) {
    for (std::size_t p = 0; p < players; ++p) {
      if (rnd() % 8 == 0) cur[p * 2] = static_cast<std::uint8_t>(rnd() % 3);
      if (rnd() % 10 == 0) cur[p * 2 + 1] = static_cast<std::uint8_t>(rnd() % 2);
    }
    std::copy(cur.begin(), cur.end(), out.begin() + static_cast<std::ptrdiff_t>(f * players * 2));
  }
  return out;
}

}  // namespace
