// The rollback core against a straight-through run of the same game: however
// the canonical inputs are delayed, reordered or repeated, the confirmed states
// must be the straight-through ones.

#include <algorithm>
#include <cstdio>
#include <map>
#include <random>

#include "check.hpp"
#include "npk/session/rollback.hpp"
#include "toy_game.hpp"

using namespace npk::session;

namespace {

struct Reference {
  std::vector<std::uint8_t> inputs;           // frames * players * 2
  std::vector<std::uint64_t> hashes;          // hash of the state at each frame, 0..frames
  std::vector<std::vector<std::uint8_t>> snapshots;
  std::size_t players;
};

Reference make_reference(std::uint64_t seed, std::size_t players, Frame frames) {
  Reference r;
  r.players = players;
  r.inputs = toy_inputs(seed, players, frames);
  ToyGame g(players);
  for (Frame f = 0; f <= frames; ++f) {
    r.hashes.push_back(g.hash());
    r.snapshots.emplace_back();
    g.snapshot(r.snapshots.back());
    if (f < frames) g.step(f, std::span(r.inputs).subspan(static_cast<std::size_t>(f) * players * 2, players * 2));
  }
  return r;
}

std::span<const std::uint8_t> frame_inputs(const Reference& r, Frame f) {
  return std::span(r.inputs).subspan(static_cast<std::size_t>(f) * r.players * 2, r.players * 2);
}

// A client's run: one tick = one local frame attempt. Canonical inputs arrive
// after a random number of ticks, in any order, some twice. The local player
// (0) uses its own input `delay` frames ahead, and with probability
// `wrong_local` gets it wrong (as if the canonical input was a late one).
struct Run {
  std::uint64_t seed;
  std::size_t players = 2;
  Frame frames = 400;
  int max_delay_ticks = 10;
  int local_delay = 2;
  double wrong_local = 0.0;
  double dup = 0.1;
  RollbackConfig cfg{};
};

void run_one(const Run& run) {
  const Reference ref = make_reference(run.seed, run.players, run.frames + 64);
  std::mt19937_64 rng(run.seed);
  ToyGame game(run.players);
  Rollback rb(game, run.cfg);
  std::multimap<int, Frame> due;  // tick -> canonical frame to deliver
  Frame next_to_send = 0;
  Frame next_local = 0;
  std::vector<std::uint8_t> wrong{2, 1};
  std::size_t checkpoints = 0;
  for (int tick = 0; tick < 20000 && !(rb.frame() >= run.frames && rb.confirmed() == rb.frame()); ++tick) {
    // The server finalises about one frame per tick, ahead of or behind the client.
    if (next_to_send < run.frames + 64 && tick >= static_cast<int>(next_to_send) / 2) {
      due.emplace(tick + static_cast<int>(rng() % (run.max_delay_ticks + 1)), next_to_send);
      if (std::uniform_real_distribution<double>(0, 1)(rng) < run.dup)
        due.emplace(tick + static_cast<int>(rng() % (run.max_delay_ticks + 1)), next_to_send);
      ++next_to_send;
    }
    for (auto it = due.begin(); it != due.end() && it->first <= tick;) {
      Frame f = it->second;
      if (rb.add_canonical(f, frame_inputs(ref, f))) it = due.erase(it);
      else { due.emplace(tick + 1, f); it = due.erase(it); }  // outside the window: try again later
    }
    // The local input for the frame `local_delay` ahead.
    while (next_local < rb.frame() + static_cast<Frame>(run.local_delay) && next_local < run.frames + 64) {
      auto in = frame_inputs(ref, next_local).subspan(0, 2);
      bool bad = std::uniform_real_distribution<double>(0, 1)(rng) < run.wrong_local;
      if (!rb.set_provisional(0, next_local, bad ? std::span<const std::uint8_t>(wrong) : in)) break;
      ++next_local;
    }
    if (rb.frame() < run.frames) rb.advance(); else rb.correct();
    CHECK(rb.ok());
    CHECK(rb.frame() - rb.confirmed() <= run.cfg.max_rollback);
    if (rb.confirmed() == rb.frame()) CHECK(rb.state_hash() == ref.hashes[rb.frame()]);
    while (auto c = rb.take_checkpoint()) {
      CHECK(c->frame % run.cfg.hash_interval == 0);
      CHECK(c->hash == ref.hashes[c->frame]);
      ++checkpoints;
    }
  }
  CHECK(rb.frame() >= run.frames);
  CHECK(rb.confirmed() == rb.frame());
  CHECK(rb.state_hash() == ref.hashes[rb.frame()]);
  CHECK(rb.stats().max_depth <= run.cfg.max_rollback);
  CHECK(checkpoints >= run.frames / run.cfg.hash_interval - 1);
}

void converges_under_any_delivery() {
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    Run r;
    r.seed = seed;
    r.players = 2 + seed % 3;
    r.max_delay_ticks = static_cast<int>(seed % 12);
    r.wrong_local = seed % 4 == 0 ? 0.2 : 0.0;
    r.local_delay = 1 + static_cast<int>(seed % 4);
    run_one(r);
  }
}

void nothing_wrong_means_no_rollback() {
  // Nobody ever changes their input, and canonical inputs arrive in time:
  // prediction is always right.
  ToyGame game(2);
  Rollback rb(game);
  const std::vector<std::uint8_t> idle{1, 0, 1, 0};
  for (Frame f = 0; f < 100; ++f) {
    CHECK(rb.add_canonical(f, idle));
    CHECK(rb.advance());
  }
  CHECK(rb.stats().rollbacks == 0);
  CHECK(rb.confirmed() == 100 && rb.frame() == 100);
}

void rollback_happens_and_is_counted() {
  ToyGame game(2);
  Rollback rb(game);
  const std::vector<std::uint8_t> idle{1, 0, 1, 0};
  for (Frame f = 0; f < 10; ++f) CHECK(rb.advance());  // all guessed
  CHECK(rb.confirmed() == 0 && rb.frame() == 10);
  const std::vector<std::uint8_t> moving{1, 0, 2, 1};
  CHECK(rb.add_canonical(3, moving));  // differs from the guess at frame 3
  rb.correct();
  CHECK(rb.stats().rollbacks == 1);
  CHECK(rb.stats().max_depth == 7);  // frames 3..9
  CHECK(rb.frames_since_rollback() == 0);
}

void stalls_at_the_limit() {
  ToyGame game(2);
  RollbackConfig cfg;
  cfg.max_rollback = 8;
  Rollback rb(game, cfg);
  int n = 0;
  while (rb.advance()) ++n;
  CHECK(n == 8);
  CHECK(!rb.can_advance());
  const std::vector<std::uint8_t> idle{1, 0, 1, 0};
  CHECK(rb.add_canonical(0, idle));
  CHECK(rb.advance());  // confirmed 0 moved the window by one
  CHECK(rb.frame() == 9 && rb.confirmed() == 1);
}

void refuses_what_it_should() {
  ToyGame game(2);
  Rollback rb(game);
  const std::vector<std::uint8_t> ok{1, 0, 1, 0};
  const std::vector<std::uint8_t> short_{1, 0, 1};
  const std::vector<std::uint8_t> bad_dir{1, 0, 7, 0};
  const std::vector<std::uint8_t> bad_jump{1, 0, 1, 2};
  CHECK(!rb.add_canonical(0, short_));
  CHECK(!rb.add_canonical(0, bad_dir));
  CHECK(!rb.add_canonical(0, bad_jump));
  CHECK(!rb.add_canonical(rb.window_end(), ok));
  CHECK(!rb.add_canonical(0xfffffff0u, ok));
  CHECK(rb.add_canonical(rb.window_end() - 1, ok));
  const std::vector<std::uint8_t> one{1, 0};
  CHECK(!rb.set_provisional(2, 0, one));  // no such player
  CHECK(!rb.set_provisional(0, 0, std::vector<std::uint8_t>{1}));
  CHECK(!rb.set_provisional(0, 0, std::vector<std::uint8_t>{3, 0}));
  CHECK(!rb.set_provisional(0, rb.window_end(), one));
  CHECK(rb.stats().rejected == 9);
  // Nothing of that changed the state.
  ToyGame fresh(2);
  CHECK(rb.state_hash() == fresh.hash() && rb.frame() == 0);
  // A frame already confirmed is accepted and ignored.
  CHECK(rb.add_canonical(0, ok));
  CHECK(rb.advance());
  CHECK(rb.confirmed() == 1);
  CHECK(rb.add_canonical(0, ok));
}

void first_canonical_wins() {
  // A repeated canonical frame with other content (a hostile or buggy server) changes nothing.
  ToyGame game(2);
  Rollback rb(game);
  const std::vector<std::uint8_t> a{1, 0, 1, 0}, b{0, 1, 2, 1};
  CHECK(rb.add_canonical(0, a));
  CHECK(rb.add_canonical(0, b));
  CHECK(rb.advance());
  CHECK(rb.confirmed() == 1);
  ToyGame g(2);
  g.step(0, a);
  CHECK(rb.state_hash() == g.hash());
}

void resync_repairs_a_diverged_state() {
  const Reference ref = make_reference(5, 2, 300);
  ToyGame good(2), bad(2);
  bad.bug_at_frame = 41;
  Rollback a(good), b(bad);
  std::map<Frame, std::uint64_t> good_cp;
  Frame detected = 0, resynced_at = 0;
  for (Frame f = 0; f < 300; ++f) {
    CHECK(a.add_canonical(f, frame_inputs(ref, f)));
    CHECK(b.add_canonical(f, frame_inputs(ref, f)));
    CHECK(a.advance());
    CHECK(b.advance());
    while (auto c = a.take_checkpoint()) good_cp[c->frame] = c->hash;
    while (auto c = b.take_checkpoint()) {
      CHECK(good_cp.count(c->frame));
      if (good_cp[c->frame] != c->hash && detected == 0) detected = c->frame;
    }
    // The server's answer comes 10 frames after the mismatching report: b has
    // moved on, so the resync takes it back to the checkpoint and it plays forward.
    if (detected != 0 && resynced_at == 0 && f == detected + 10) {
      resynced_at = f;
      CHECK(detected > 41);
      CHECK(b.resync(detected, ref.snapshots[detected]));
      CHECK(b.frame() == detected && b.confirmed() == detected);
      // The frames it had already confirmed are gone; the session asks the server again
      // (next_canonical_needed), here they are simply given.
      CHECK(b.next_canonical_needed() == detected);
      for (Frame g = detected; g <= f; ++g) CHECK(b.add_canonical(g, frame_inputs(ref, g)));
      while (b.frame() <= f) CHECK(b.advance());
    }
  }
  CHECK(resynced_at != 0);
  CHECK(b.stats().resyncs == 1);
  CHECK(a.state_hash() == b.state_hash());
  CHECK(a.frame() == b.frame());
}

void resync_with_a_bad_snapshot_ends_the_session() {
  ToyGame game(2);
  Rollback rb(game);
  CHECK(!rb.resync(30, std::vector<std::uint8_t>(5, 0)));
  CHECK(!rb.ok());
  CHECK(!rb.advance());
}

}  // namespace

int main() {
  converges_under_any_delivery();
  nothing_wrong_means_no_rollback();
  rollback_happens_and_is_counted();
  stalls_at_the_limit();
  refuses_what_it_should();
  first_canonical_wins();
  resync_repairs_a_diverged_state();
  resync_with_a_bad_snapshot_ends_the_session();
  std::puts("rollback_test: ok");
  return 0;
}
