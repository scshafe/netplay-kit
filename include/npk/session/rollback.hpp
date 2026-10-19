#pragma once

// The rollback core: runs a Simulation ahead of the canonical inputs and puts
// it right when they arrive. It knows nothing of the network.
//
// Terms. The server fixes one canonical input per player and frame. A frame is
// *confirmed* once it was simulated with the canonical inputs of every player.
// `frame()` is the next frame to simulate, `confirmed()` the first frame that is
// not confirmed, so frames [confirmed, frame) were simulated with some inputs
// still guessed. The local player's own input is *provisional* until its
// canonical version arrives: it is used at once and normally turns out to be
// the canonical one. A player's missing input is predicted from their previous
// one (Simulation::predict_input).
//
// When a canonical or provisional input arrives for a frame that was already
// simulated with a different input, the state is restored from the snapshot at
// the earliest such frame and the frames after it are simulated again
// (`correct()`). The simulation never runs more than `max_rollback` frames past
// the confirmed ones, which bounds the rollback depth, the snapshot memory, and
// how long a stalled peer can hold the others' prediction open.
//
// All inputs are bytes of the game's encoding and are validated on the way in:
// wrong size, outside the window, or rejected by Simulation::valid_input.

#include <cstddef>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "npk/session/simulation.hpp"

namespace npk::session {

struct RollbackConfig {
  std::size_t max_rollback = 16;  // frames simulated beyond the confirmed ones, at most
  std::size_t window = 64;        // frames from `confirmed` for which inputs are kept; > max_rollback + 8
  Frame hash_interval = 30;       // a checkpoint every this many frames; 0: none
};

struct RollbackStats {
  std::uint64_t rollbacks = 0;
  std::uint64_t resimulated_frames = 0;
  std::uint64_t max_depth = 0;     // the most frames re-simulated by one rollback
  std::uint64_t rejected = 0;      // inputs refused as malformed or outside the window
  std::uint64_t resyncs = 0;
};

// The hash of the state at a confirmed frame that is a multiple of hash_interval.
struct Checkpoint {
  Frame frame;
  std::uint64_t hash;
};

class Rollback {
 public:
  explicit Rollback(Simulation& sim, const RollbackConfig& config = {});

  Frame frame() const { return frame_; }
  Frame confirmed() const { return confirmed_; }
  // False if restoring a snapshot failed; the session must end.
  bool ok() const { return ok_; }
  // The first frame inputs are not accepted for.
  Frame window_end() const { return confirmed_ + static_cast<Frame>(cfg_.window); }
  bool can_advance() const { return ok_ && frame_ - confirmed_ < cfg_.max_rollback; }
  // Frames simulated since the last rollback (saturating).
  std::uint32_t frames_since_rollback() const { return since_rollback_; }

  // A player's input, to be used for a frame until the canonical one arrives.
  // False (and counted) if it is malformed or outside [confirmed, window_end).
  bool set_provisional(PlayerIndex player, Frame frame, std::span<const std::uint8_t> input);
  // The canonical inputs of all players for a frame, player_count() * input_size()
  // bytes. A frame already confirmed is ignored and counts as accepted; a frame
  // already held is ignored too (the first canonical input wins).
  bool add_canonical(Frame frame, std::span<const std::uint8_t> inputs);
  bool has_canonical(Frame frame) const;
  // The first frame from confirmed() on for which no canonical input is held.
  Frame next_canonical_needed() const;

  // Simulates again from the earliest frame whose inputs turned out wrong, then
  // advances confirmed() as far as canonical inputs allow.
  void correct();
  // correct(), then simulates one more frame if can_advance(). Returns whether it did.
  bool advance();

  // Replaces the state with the server's: `snapshot` is the state at `frame`.
  // Frames before it are dropped, canonical inputs from it on are kept. After
  // this confirmed() == frame() == `frame`. False if the snapshot is malformed.
  bool resync(Frame frame, std::span<const std::uint8_t> snapshot);

  std::optional<Checkpoint> take_checkpoint();
  std::uint64_t state_hash() const { return sim_.hash(); }
  const RollbackStats& stats() const { return stats_; }

 private:
  struct Slot {
    Frame tag = 0;
    bool claimed = false;
    std::vector<std::uint8_t> snapshot;   // the state at `tag`
    bool has_snapshot = false;
    std::uint64_t hash = 0;               // of that state, when tag is a checkpoint
    std::vector<std::uint8_t> used;       // inputs simulated for frame `tag`
    bool has_used = false;
    std::vector<std::uint8_t> canonical;
    bool has_canonical = false;
    std::vector<std::uint8_t> provisional;  // player_count * input_size
    std::vector<std::uint8_t> has_prov;     // per player
  };

  Slot& claim(Frame f);
  Slot* find(Frame f);
  const Slot* find(Frame f) const;
  std::size_t index(Frame f) const { return f % cfg_.window; }
  bool in_window(Frame f) const { return f >= confirmed_ && f - confirmed_ < cfg_.window; }
  void mark_mismatch(Frame f);
  void resolve(Frame f, std::vector<std::uint8_t>& out) const;
  void simulate(Frame f);  // resolves and steps frame f == frame_, stores the snapshot of f + 1
  void store_state(Frame f);
  void advance_confirmed();

  Simulation& sim_;
  RollbackConfig cfg_;
  std::size_t players_, in_size_, frame_bytes_;
  std::vector<Slot> slots_;
  Frame frame_ = 0, confirmed_ = 0;
  std::vector<std::uint8_t> prev_confirmed_;  // the inputs of frame confirmed - 1
  std::optional<Frame> rollback_from_;
  std::uint32_t since_rollback_ = 0;
  bool ok_ = true;
  std::deque<Checkpoint> checkpoints_;
  RollbackStats stats_;
  mutable std::vector<std::uint8_t> scratch_;
};

}  // namespace npk::session
