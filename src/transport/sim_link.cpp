#include "npk/transport/sim_link.hpp"

#include <algorithm>
#include <cstring>

namespace npk::transport {

bool SimLink::send(const Address& to, std::span<const std::uint8_t> data) {
  ++stats_.sent_datagrams;
  stats_.sent_bytes += data.size();
  net_.transmit(addr_, to, data);
  return true;
}

std::optional<std::size_t> SimLink::receive(Address& from, std::span<std::uint8_t> buf) {
  if (inbox_.empty()) return std::nullopt;
  Item item = std::move(inbox_.front());
  inbox_.pop_front();
  const std::size_t n = std::min(buf.size(), item.data.size());
  if (n > 0) std::memcpy(buf.data(), item.data.data(), n);
  from = item.from;
  ++stats_.received_datagrams;
  stats_.received_bytes += n;
  return n;
}

std::unique_ptr<SimLink> SimNetwork::make_link(const Address& addr) {
  struct Owned : SimLink {
    Owned(SimNetwork& n, Address a) : SimLink(n, a) {}
    ~Owned() override { net_.unregister(addr_); }
  };
  auto link = std::make_unique<Owned>(*this, addr);
  links_[addr] = link.get();
  return link;
}

void SimNetwork::inject(const Address& from, const Address& to,
                        std::span<const std::uint8_t> data) {
  transmit(from, to, data);
}

bool SimNetwork::chance(double p) {
  if (p <= 0) return false;
  if (p >= 1) return true;
  // 53 random bits to [0,1); not std::uniform_real_distribution, whose output differs
  // between standard libraries and would make runs differ between Linux and macOS.
  return static_cast<double>(rng_() >> 11) * (1.0 / 9007199254740992.0) < p;
}

Micros SimNetwork::uniform(Micros max) {
  if (max <= 0) return 0;
  return static_cast<Micros>(rng_() % (static_cast<std::uint64_t>(max) + 1));
}

void SimNetwork::transmit(const Address& from, const Address& to,
                          std::span<const std::uint8_t> data) {
  if (data.size() > max_payload_) return;
  const auto it = paths_.find({from, to});
  const LinkConditions& c = it != paths_.end() ? it->second : default_;
  if (chance(c.drop)) return;
  auto arrival = [&] {
    Micros at = now_ + c.delay + uniform(c.jitter);
    if (chance(c.reorder)) at += c.reorder_delay;
    return at;
  };
  enqueue(arrival(), from, to, data);
  if (chance(c.duplicate)) enqueue(arrival(), from, to, data);
}

void SimNetwork::enqueue(Micros at, const Address& from, const Address& to,
                         std::span<const std::uint8_t> data) {
  in_flight_.emplace(at, InFlight{from, to, {data.begin(), data.end()}});
}

void SimNetwork::advance(Micros dt) {
  now_ += dt;
  while (!in_flight_.empty() && in_flight_.begin()->first <= now_) {
    InFlight f = std::move(in_flight_.begin()->second);
    in_flight_.erase(in_flight_.begin());
    const auto link = links_.find(f.to);
    if (link == links_.end()) continue;  // nobody there
    auto& inbox = link->second->inbox_;
    if (inbox.size() >= inbox_limit_) continue;
    inbox.push_back({f.from, std::move(f.data)});
  }
}

}  // namespace npk::transport
