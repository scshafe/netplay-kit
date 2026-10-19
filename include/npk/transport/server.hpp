#pragma once

// The server end of the transport: answers handshakes, tracks connections.
//
// The handshake (docs/design.md, "Security"):
//   Init       client -> server  client_nonce, padded to >= 128 bytes
//   Challenge  server -> client  client_nonce, bucket, cookie        (stateless)
//   Response   client -> server  the same fields echoed, padded
//   Accept     server -> client  client_nonce, server time; header carries the connection id
// The cookie is a SipHash of the client's address, its nonce and a time bucket
// under a server secret. Until a Response proves the client can receive at its
// address, the server keeps no state for it, and no reply is ever larger than
// the request that caused it.

#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>

#include "npk/transport/connection.hpp"
#include "npk/transport/link.hpp"
#include "npk/transport/ratelimit.hpp"
#include "npk/transport/siphash.hpp"
#include "npk/transport/time.hpp"
#include "npk/transport/types.hpp"
#include "npk/transport/wire.hpp"

namespace npk::transport {

struct ServerConfig {
  std::uint64_t seed = 0;  // 0: secrets from the OS; non-zero: repeatable, for tests
  std::size_t max_connections = 64;
  Micros cookie_bucket = 10'000'000;  // a cookie is valid for its bucket and the next
  Micros idle_timeout = 10'000'000;
  // Init packets, which cost a reply: per source IP address, and in all.
  double init_rate_per_address = 5;  // per second
  double init_burst_per_address = 10;
  double init_rate_total = 2000;
  double init_burst_total = 2000;
  std::size_t max_receive_per_update = 4096;
  ConnectionConfig connection{};
};

struct ServerStats {
  std::uint64_t malformed = 0;       // failed to parse, or an invalid source
  std::uint64_t rate_limited = 0;    // Init dropped by a limit
  std::uint64_t bad_cookie = 0;      // Response with a wrong or expired cookie
  std::uint64_t server_full = 0;     // valid Response, no room
  std::uint64_t challenges_sent = 0;
  std::uint64_t accepts_sent = 0;
  std::uint64_t amplification_blocked = 0;  // a reply that would have been larger than its request
  std::uint64_t unknown_connection = 0;     // Data for no connection, or from the wrong address
};

struct ServerEvent {
  enum class Type : std::uint8_t { Connected, Disconnected } type;
  ConnectionId id;
  Address address;
  DisconnectReason reason = DisconnectReason::Closed;  // Disconnected only
};

struct ServerMessage {
  ConnectionId id;
  Message message;
};

class Server {
 public:
  Server(Link& link, const ServerConfig& config = {});
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Reads what has arrived, answers it, and expires idle connections.
  void update(Micros now);

  // Events and messages must be polled; they queue until taken. Messages that
  // arrived before a connection ended are still delivered after its event.
  std::optional<ServerEvent> poll_event();
  std::optional<ServerMessage> poll_message();

  // False when the connection is unknown, the message is too big, or (reliable)
  // the connection's send buffer is full.
  bool send_unreliable(ConnectionId id, std::span<const std::uint8_t> data);
  bool send_reliable(ConnectionId id, std::span<const std::uint8_t> data);

  // Ends a connection, telling the peer.
  void disconnect(ConnectionId id, DisconnectReason reason = DisconnectReason::Closed);

  std::optional<RttEstimator> rtt(ConnectionId id) const {
    const auto it = peers_.find(id);
    return it == peers_.end() ? std::nullopt : std::optional(it->second.conn.rtt());
  }
  std::size_t connection_count() const { return peers_.size(); }
  const ServerStats& stats() const { return stats_; }

 private:
  struct Peer {
    ConnectionId id;
    Address address;
    std::uint64_t client_nonce;
    Connection conn;
  };

  void handle(const Address& from, std::span<const std::uint8_t> datagram);
  void handle_init(const Address& from, std::size_t request_size, const Packet& p);
  void handle_response(const Address& from, std::size_t request_size, const Packet& p);
  void handle_data(const Address& from, const Packet& p);
  void send_reply(const Address& to, const PacketBuilder& b, std::size_t request_size);
  void remove(std::unordered_map<ConnectionId, Peer>::iterator it, DisconnectReason reason);
  std::uint32_t bucket_now() const;
  std::uint64_t cookie_for(const Address& a, std::uint64_t nonce, std::uint32_t bucket) const;
  std::uint64_t ip_hash(const Address& a) const;
  ConnectionId new_id();

  Link& link_;
  ServerConfig cfg_;
  SipKey cookie_key_, id_key_, rate_key_;
  BucketTable per_address_;
  TokenBucket total_;
  Micros now_ = 0;
  std::uint64_t id_counter_ = 0;
  std::unordered_map<ConnectionId, Peer> peers_;
  std::unordered_map<Address, ConnectionId> by_address_;
  std::deque<ServerEvent> events_;
  std::deque<ServerMessage> orphaned_;  // from connections that have ended
  std::size_t poll_cursor_ = 0;
  ServerStats stats_;
  std::array<std::uint8_t, kMaxDatagram + 1> rx_{};
};

}  // namespace npk::transport
