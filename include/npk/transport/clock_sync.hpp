#pragma once

#include <algorithm>
#include <array>
#include <cstdlib>

#include "npk/transport/time.hpp"

namespace npk::transport {

// What this end knows about the peer's clock and the path to it, from timed
// ping/pong exchanges (NTP's four timestamps).
//
// A ping sent at local t1 reaches the peer at its t2; the peer's pong leaves at
// its t3 and arrives at local t4. Then
//   rtt    = (t4 - t1) - (t3 - t2)           (the peer's holding time removed)
//   offset = ((t2 - t1) + (t3 - t4)) / 2     (peer clock minus local clock)
// The offset is exact when the two directions take equally long; when they do
// not it is off by half the difference, which no ping can reveal. Of the last
// kWindow samples, the one with the lowest round trip is the one least disturbed
// by queueing, so that sample gives the offset. Old samples age out, so a drifting
// clock is followed.
class ClockSync {
 public:
  static constexpr std::size_t kWindow = 16;

  void add(Micros rtt, Micros offset) {
    rtt = std::max<Micros>(rtt, 0);
    samples_[next_ % kWindow] = {rtt, offset};
    ++next_;
    if (count_ == 0) {
      srtt_ = rtt;
      rttvar_ = rtt / 2;
    } else {
      rttvar_ = (3 * rttvar_ + std::abs(srtt_ - rtt)) / 4;
      srtt_ = (7 * srtt_ + rtt) / 8;
    }
    ++count_;
  }

  bool has_estimate() const { return count_ > 0; }
  std::size_t sample_count() const { return count_; }  // all time, not just the window

  // Peer clock minus local clock, from the lowest-round-trip recent sample.
  Micros offset() const { return best().offset; }
  // The lowest round trip among the recent samples.
  Micros min_rtt() const { return best().rtt; }
  // Smoothed round trip (weight 1/8 on each new sample) and its mean deviation.
  Micros rtt() const { return srtt_; }
  Micros jitter() const { return rttvar_; }

  Micros to_local(Micros peer_time) const { return peer_time - offset(); }
  Micros to_peer(Micros local_time) const { return local_time + offset(); }

 private:
  struct Sample {
    Micros rtt = 0, offset = 0;
  };
  const Sample& best() const {
    const std::size_t n = std::min(count_, kWindow);
    std::size_t best = 0;
    for (std::size_t i = 1; i < n; ++i) {
      if (samples_[i].rtt < samples_[best].rtt) best = i;
    }
    return samples_[best];
  }

  std::array<Sample, kWindow> samples_{};
  std::size_t next_ = 0, count_ = 0;
  Micros srtt_ = 0, rttvar_ = 0;
};

}  // namespace npk::transport
