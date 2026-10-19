#pragma once

#include <algorithm>
#include <cstdlib>

#include "npk/transport/time.hpp"

namespace npk::transport {

// Smoothed round-trip time and the retransmission timeout derived from it, as in
// RFC 6298 (integer arithmetic, microseconds).
class RttEstimator {
 public:
  explicit RttEstimator(Micros initial_rto = 250'000, Micros min_rto = 50'000,
                        Micros max_rto = 3'000'000)
      : rto_(initial_rto), min_rto_(min_rto), max_rto_(max_rto) {}

  void sample(Micros rtt) {
    rtt = std::max<Micros>(rtt, 0);
    if (!has_sample_) {
      srtt_ = rtt;
      rttvar_ = rtt / 2;
      has_sample_ = true;
    } else {
      rttvar_ = (3 * rttvar_ + std::abs(srtt_ - rtt)) / 4;
      srtt_ = (7 * srtt_ + rtt) / 8;
    }
    rto_ = std::clamp<Micros>(srtt_ + std::max<Micros>(4 * rttvar_, 1000), min_rto_, max_rto_);
  }

  bool has_sample() const { return has_sample_; }
  Micros srtt() const { return srtt_; }
  Micros rttvar() const { return rttvar_; }
  Micros rto() const { return rto_; }
  Micros max_rto() const { return max_rto_; }

 private:
  bool has_sample_ = false;
  Micros srtt_ = 0, rttvar_ = 0, rto_, min_rto_, max_rto_;
};

}  // namespace npk::transport
