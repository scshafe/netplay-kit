#include "npk/transport/server.hpp"

#include <cstring>

#include "entropy.hpp"

namespace npk::transport {
namespace {

constexpr std::size_t kRateSlots = 1024;

}  // namespace

Server::Server(Link& link, const ServerConfig& config)
    : link_(link),
      cfg_(config),
      per_address_(kRateSlots, config.init_rate_per_address, config.init_burst_per_address),
      total_(config.init_rate_total, config.init_burst_total) {
  Entropy e(config.seed);
  cookie_key_ = e.key();
  id_key_ = e.key();
  rate_key_ = e.key();
}

Server::~Server() = default;

std::optional<ServerEvent> Server::poll_event() {
  if (events_.empty()) return std::nullopt;
  ServerEvent e = events_.front();
  events_.pop_front();
  return e;
}

std::uint32_t Server::bucket_now() const {
  return static_cast<std::uint32_t>(now_ / cfg_.cookie_bucket);
}

std::uint64_t Server::cookie_for(const Address& a, std::uint64_t nonce, std::uint32_t bucket) const {
  std::array<std::uint8_t, 16 + 2 + 8 + 4> m{};
  std::memcpy(m.data(), a.ip.data(), 16);
  m[16] = static_cast<std::uint8_t>(a.port >> 8);
  m[17] = static_cast<std::uint8_t>(a.port);
  for (int i = 0; i < 8; ++i) m[18 + i] = static_cast<std::uint8_t>(nonce >> (8 * i));
  for (int i = 0; i < 4; ++i) m[26 + i] = static_cast<std::uint8_t>(bucket >> (8 * i));
  return siphash24(cookie_key_, m);
}

std::uint64_t Server::ip_hash(const Address& a) const { return siphash24(rate_key_, a.ip); }

ConnectionId Server::new_id() {
  for (;;) {
    std::array<std::uint8_t, 8> c{};
    const std::uint64_t n = ++id_counter_;
    for (int i = 0; i < 8; ++i) c[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(n >> (8 * i));
    const auto id = static_cast<ConnectionId>(siphash24(id_key_, c));
    if (id != 0 && !peers_.contains(id)) return id;
  }
}

void Server::update(Micros now) {
  now_ = now;
  Address from;
  for (std::size_t i = 0; i < cfg_.max_receive_per_update; ++i) {
    const auto n = link_.receive(from, rx_);
    if (!n) break;
    handle(from, {rx_.data(), *n});
  }
  for (auto it = peers_.begin(); it != peers_.end();) {
    if (now_ - it->second.last_heard > cfg_.idle_timeout) {
      auto next = std::next(it);
      remove(it, DisconnectReason::Timeout);
      it = next;
    } else {
      ++it;
    }
  }
}

void Server::handle(const Address& from, std::span<const std::uint8_t> datagram) {
  Packet p;
  if (from.port == 0 || parse_packet(datagram, p) != ParseError::None) {
    ++stats_.malformed;
    return;
  }
  switch (p.header.type) {
    case PacketType::Init: handle_init(from, datagram.size(), p); break;
    case PacketType::Response: handle_response(from, datagram.size(), p); break;
    case PacketType::Data: handle_data(from, p); break;
    default: ++stats_.malformed; break;  // Challenge and Accept are for clients
  }
}

void Server::handle_init(const Address& from, std::size_t request_size, const Packet& p) {
  std::uint64_t nonce;
  if (parse_init(p.body, nonce) != ParseError::None) {
    ++stats_.malformed;
    return;
  }
  if (!per_address_.take(ip_hash(from), now_) || !total_.take(now_)) {
    ++stats_.rate_limited;
    return;
  }
  const std::uint32_t bucket = bucket_now();
  PacketBuilder b(Header{PacketType::Challenge});
  b.challenge({nonce, bucket, cookie_for(from, nonce, bucket)});
  ++stats_.challenges_sent;
  send_reply(from, b, request_size);
}

void Server::handle_response(const Address& from, std::size_t request_size, const Packet& p) {
  ChallengeBody r;
  if (parse_response(p.body, r) != ParseError::None) {
    ++stats_.malformed;
    return;
  }
  const std::uint32_t now_bucket = bucket_now();
  const bool fresh = r.bucket == now_bucket || r.bucket + 1 == now_bucket;
  if (!fresh || r.cookie != cookie_for(from, r.client_nonce, r.bucket)) {
    ++stats_.bad_cookie;
    return;
  }

  ConnectionId id = 0;
  if (const auto existing = by_address_.find(from); existing != by_address_.end()) {
    auto peer = peers_.find(existing->second);
    if (peer->second.client_nonce == r.client_nonce) {
      id = peer->second.id;  // a repeated Response: our Accept was lost
      peer->second.last_heard = now_;
    } else {
      remove(peer, DisconnectReason::Replaced);  // the client restarted
    }
  }
  if (id == 0) {
    if (peers_.size() >= cfg_.max_connections) {
      ++stats_.server_full;
      return;
    }
    id = new_id();
    peers_.emplace(id, Peer{id, from, r.client_nonce, now_});
    by_address_[from] = id;
    events_.push_back({ServerEvent::Type::Connected, id, from});
  }
  PacketBuilder b(Header{PacketType::Accept, id});
  b.accept({r.client_nonce, static_cast<std::uint64_t>(now_)});
  ++stats_.accepts_sent;
  send_reply(from, b, request_size);
}

void Server::handle_data(const Address& from, const Packet& p) {
  const auto it = peers_.find(p.header.connection);
  if (it == peers_.end() || it->second.address != from) {
    ++stats_.unknown_connection;
    return;
  }
  Peer& peer = it->second;
  peer.last_heard = now_;
  FrameReader r(p.body);
  Frame f;
  while (!r.done()) {
    if (r.next(f) != ParseError::None) break;
    if (f.kind == FrameKind::Close) {
      remove(it, DisconnectReason::Closed);  // whatever the peer's reason, it left
      return;
    }
  }
}

void Server::send_reply(const Address& to, const PacketBuilder& b, std::size_t request_size) {
  if (b.bytes().size() > request_size) {
    ++stats_.amplification_blocked;
    return;
  }
  link_.send(to, b.bytes());
}

void Server::send_close(const Peer& peer, DisconnectReason reason) {
  Header h{PacketType::Data, peer.id};
  PacketBuilder b(h);
  b.add_close(static_cast<std::uint8_t>(reason));
  link_.send(peer.address, b.bytes());
}

void Server::remove(std::unordered_map<ConnectionId, Peer>::iterator it, DisconnectReason reason) {
  const Peer peer = it->second;
  peers_.erase(it);
  by_address_.erase(peer.address);
  events_.push_back({ServerEvent::Type::Disconnected, peer.id, peer.address, reason});
}

void Server::disconnect(ConnectionId id, DisconnectReason reason) {
  const auto it = peers_.find(id);
  if (it == peers_.end()) return;
  send_close(it->second, reason);
  remove(it, reason);
}

}  // namespace npk::transport
