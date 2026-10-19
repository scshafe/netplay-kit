#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "npk/transport/time.hpp"

namespace npk::transport {

// A token bucket: `rate` events per second with bursts up to `burst`.
class TokenBucket {
 public:
  TokenBucket(double rate_per_s = 1, double burst = 1) : rate_(rate_per_s), burst_(burst), tokens_(burst) {}

  bool take(Micros now) {
    if (started_ && now > last_) {
      tokens_ = std::min(burst_, tokens_ + rate_ * static_cast<double>(now - last_) * 1e-6);
    }
    if (!started_ || now > last_) last_ = now;
    started_ = true;
    if (tokens_ < 1) return false;
    tokens_ -= 1;
    return true;
  }

 private:
  double rate_, burst_, tokens_;
  Micros last_ = 0;
  bool started_ = false;
};

// A fixed table of buckets picked by a key's hash: memory stays constant however
// many sources there are. Sources that collide share a bucket, which can only
// make the limit stricter for them, never let more through.
class BucketTable {
 public:
  BucketTable(std::size_t slots, double rate_per_s, double burst)
      : buckets_(slots, TokenBucket(rate_per_s, burst)) {}
  bool take(std::uint64_t hash, Micros now) { return buckets_[hash % buckets_.size()].take(now); }

 private:
  std::vector<TokenBucket> buckets_;
};

}  // namespace npk::transport
