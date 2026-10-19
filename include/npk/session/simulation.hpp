#pragma once

// The interface a game implements to plug into netplay-kit. Nothing in the
// framework names a game: it sees encoded inputs, opaque snapshots and a hash.
//
// The simulation must be deterministic. Given the same state and the same
// inputs, step() must produce the same state on every machine and every run:
// no clocks, no unseeded randomness, no iteration over unordered containers,
// and floating point only if it is bit-identical across the builds in play
// (fixed-point is the safe choice). hash() must depend on all of the state
// that step() reads, and on nothing else.
//
// Frames are numbered from 0. "The state at frame f" is the state before
// step(f, ...) runs, so a new simulation is at frame 0.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace npk::session {

using Frame = std::uint32_t;
using PlayerIndex = std::uint8_t;

class Simulation {
 public:
  virtual ~Simulation() = default;

  // Fixed for the life of the simulation.
  virtual std::size_t player_count() const = 0;
  // Bytes of one player's encoded input, the same for every player and frame.
  virtual std::size_t input_size() const = 0;

  // Advances from `frame` to `frame + 1`. `inputs` is player_count() inputs of
  // input_size() bytes each, in player order. Every one passed valid_input().
  virtual void step(Frame frame, std::span<const std::uint8_t> inputs) = 0;

  // Replaces the contents of `out` with the whole state.
  virtual void snapshot(std::vector<std::uint8_t>& out) const = 0;
  // Sets the state from a snapshot() of the same game. A snapshot can come from
  // the network, so check it; false means it was malformed, and the state is
  // then unspecified (the session ends).
  virtual bool restore(std::span<const std::uint8_t> snapshot) = 0;

  virtual std::uint64_t hash() const = 0;

  // Inputs come from the network too. False for bytes that step() must not see
  // (an out-of-range direction, say); the server then uses the player's
  // previous input instead.
  virtual bool valid_input(std::span<const std::uint8_t> input) const { (void)input; return true; }
  // Whether the game is over. The server checks after every frame; when true it
  // ends the match and reports result_json() to the lobby. Default: never (the
  // match ends when everyone has left).
  virtual bool finished() const { return false; }
  // The game's own account of how it ended, as a JSON document (for example the
  // winner and the scores), reported to the lobby. Default: "null". Keep it small.
  virtual std::string result_json() const { return "null"; }

  // The input before anyone has pressed anything. Default: all zero bytes.
  virtual void neutral_input(std::span<std::uint8_t> out) const;
  // What to guess for a player whose input has not arrived, from their previous
  // input. Default: the same again, which is right most of the time. A game can
  // override it, for example to release a one-frame button press.
  virtual void predict_input(std::span<const std::uint8_t> previous, std::span<std::uint8_t> out) const;
};

}  // namespace npk::session
