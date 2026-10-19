#pragma once

// How many frames a player's input is delayed before it takes effect.
//
// A delay lets the local input travel to the server before the frame that uses
// it is simulated, so the server (and through it the other players) usually has
// it in time, and less is mispredicted. Rollback covers the rest. The rule:
//
//   delay = ceil((smoothed RTT / 2 + k * jitter) / frame time), clamped to [min, max]
//
// with k = 2 and a clamp of 1..6 frames at 60 Hz by default. Jitter is the mean
// deviation of the round trip (ClockSync::jitter()). The delay is set when the
// match starts and re-evaluated about twice a second at a safe point: a frame
// boundary outside any resimulation. It changes only with hysteresis: a raise
// takes `hysteresis` evaluations in a row that want one, and then goes to the
// target; a drop takes the same and also `calm_frames` frames without a
// rollback, and goes down one frame at a time. A delay that flaps would make the
// controls feel uneven, and the cost of one frame too many is small.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "npk/transport/time.hpp"

namespace npk::session {

struct InputDelayConfig {
  int tick_rate_hz = 60;
  int min_frames = 1;
  int max_frames = 6;
  double jitter_k = 2.0;
  int hysteresis = 3;
  std::uint32_t calm_frames = 60;
};

// The rule alone.
inline int target_input_delay(transport::Micros srtt, transport::Micros jitter, const InputDelayConfig& c) {
  const double frame_us = 1'000'000.0 / c.tick_rate_hz;
  const double want_us = static_cast<double>(srtt) / 2.0 + c.jitter_k * static_cast<double>(jitter);
  const int frames = static_cast<int>(std::ceil(std::max(want_us, 0.0) / frame_us));
  return std::clamp(frames, c.min_frames, c.max_frames);
}

class InputDelay {
 public:
  explicit InputDelay(const InputDelayConfig& c = {}) : cfg_(c), current_(c.min_frames) {}

  int current() const { return current_; }

  // At match start: take the target outright.
  void start(transport::Micros srtt, transport::Micros jitter) {
    current_ = target_input_delay(srtt, jitter, cfg_);
    streak_ = 0;
  }

  // At a safe point. `frames_since_rollback` counts the frames simulated since
  // the last rollback. Returns the delay to use from now on.
  int evaluate(transport::Micros srtt, transport::Micros jitter, std::uint32_t frames_since_rollback) {
    const int target = target_input_delay(srtt, jitter, cfg_);
    const int dir = target > current_ ? 1 : target < current_ ? -1 : 0;
    if (dir == 0 || (dir < 0 && frames_since_rollback < cfg_.calm_frames)) {
      streak_ = 0;
      return current_;
    }
    streak_ = (dir == streak_dir_) ? streak_ + 1 : 1;
    streak_dir_ = dir;
    if (streak_ >= cfg_.hysteresis) {
      current_ = dir > 0 ? target : current_ - 1;
      streak_ = 0;
    }
    return current_;
  }

 private:
  InputDelayConfig cfg_;
  int current_;
  int streak_ = 0, streak_dir_ = 0;
};

}  // namespace npk::session
