#pragma once

// An in-process network for tests: Links that exchange datagrams through
// simulated conditions (drop, delay, jitter, reorder, duplicate) on a virtual
// clock. Everything is driven by a seed and by advance(), so a run is exactly
// repeatable, and it needs no sockets and no sleeping.

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <vector>

#include "npk/transport/link.hpp"
#include "npk/transport/time.hpp"

namespace npk::transport {

struct LinkConditions {
  double drop = 0;            // probability a datagram is lost
  Micros delay = 0;           // fixed one-way delay
  Micros jitter = 0;          // plus uniform [0, jitter]
  double reorder = 0;         // probability of holding a datagram back by reorder_delay
  Micros reorder_delay = 0;
  double duplicate = 0;       // probability of delivering a second copy
};

struct LinkStats {
  std::uint64_t sent_datagrams = 0;
  std::uint64_t sent_bytes = 0;
  std::uint64_t received_datagrams = 0;
  std::uint64_t received_bytes = 0;
};

class SimNetwork;

class SimLink : public Link {
 public:
  Address local_address() const override { return addr_; }
  bool send(const Address& to, std::span<const std::uint8_t> data) override;
  std::optional<std::size_t> receive(Address& from, std::span<std::uint8_t> buf) override;
  const LinkStats& stats() const { return stats_; }

 protected:
  SimLink(SimNetwork& net, Address addr) : net_(net), addr_(addr) {}
  SimNetwork& net_;
  Address addr_;

 private:
  friend class SimNetwork;
  struct Item {
    Address from;
    std::vector<std::uint8_t> data;
  };
  std::deque<Item> inbox_;
  LinkStats stats_;
};

class SimNetwork {
 public:
  explicit SimNetwork(std::uint64_t seed) : rng_(seed) {}
  SimNetwork(const SimNetwork&) = delete;
  SimNetwork& operator=(const SimNetwork&) = delete;

  // Datagrams larger than this are dropped, as a path with a smaller MTU would
  // (1280 - 40 IPv6 - 8 UDP).
  void set_max_payload(std::size_t n) { max_payload_ = n; }
  // A receiver holds at most this many datagrams; more are dropped, like a full socket buffer.
  void set_inbox_limit(std::size_t n) { inbox_limit_ = n; }

  // Conditions for every path without its own.
  void set_default_conditions(const LinkConditions& c) { default_ = c; }
  // Conditions for the one-way path from -> to.
  void set_conditions(const Address& from, const Address& to, const LinkConditions& c) {
    paths_[{from, to}] = c;
  }

  // The link is the endpoint at addr. Destroy every link before the network.
  std::unique_ptr<SimLink> make_link(const Address& addr);

  // Sends a datagram as if from `from`, which need not have a link: a spoofed source.
  void inject(const Address& from, const Address& to, std::span<const std::uint8_t> data);

  Micros now() const { return now_; }
  // Moves the clock forward and delivers every datagram that has arrived by then
  // (advance(0) delivers those with no delay).
  void advance(Micros dt);

 private:
  friend class SimLink;
  struct InFlight {
    Address from, to;
    std::vector<std::uint8_t> data;
  };

  void transmit(const Address& from, const Address& to, std::span<const std::uint8_t> data);
  void enqueue(Micros at, const Address& from, const Address& to,
               std::span<const std::uint8_t> data);
  bool chance(double p);
  Micros uniform(Micros max);
  void unregister(const Address& a) { links_.erase(a); }

  std::mt19937_64 rng_;
  Micros now_ = 0;
  std::size_t max_payload_ = 1232;
  std::size_t inbox_limit_ = 4096;
  LinkConditions default_;
  std::map<std::pair<Address, Address>, LinkConditions> paths_;
  std::map<Address, SimLink*> links_;
  // Ordered by arrival time, then by the order they were sent.
  std::multimap<Micros, InFlight> in_flight_;
};

}  // namespace npk::transport
