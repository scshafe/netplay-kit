#pragma once

// The client end of the transport: runs the handshake against one server, then
// holds the connection. See server.hpp for the handshake.

#include <optional>

#include "npk/transport/connection.hpp"
#include "npk/transport/link.hpp"
#include "npk/transport/time.hpp"
#include "npk/transport/types.hpp"
#include "npk/transport/wire.hpp"

namespace npk::transport {

struct ClientConfig {
  std::uint64_t seed = 0;  // 0: the nonce comes from the OS; non-zero: repeatable, for tests
  Micros retry_interval = 250'000;      // resend the current handshake packet this often
  Micros handshake_timeout = 5'000'000;  // give up after this long
  Micros idle_timeout = 10'000'000;      // connected, but nothing heard this long
  ConnectionConfig connection{};
};

class Client {
 public:
  enum class State : std::uint8_t {
    Idle,        // connect() not called yet
    Connecting,  // handshake in progress
    Connected,
    Failed,      // the handshake did not finish
    Closed,      // was connected; see close_reason()
  };

  Client(Link& link, const Address& server, const ClientConfig& config = {});

  // Starts the handshake. Only from Idle, Failed or Closed.
  void connect(Micros now);
  void update(Micros now);
  // Tells the server and closes.
  void disconnect(DisconnectReason reason = DisconnectReason::Closed);

  // False when not connected, the message is too big, or (reliable) the buffer is full.
  bool send_unreliable(std::span<const std::uint8_t> data);
  bool send_reliable(std::span<const std::uint8_t> data);
  // Round-trip estimate of the connection, if there is one.
  std::optional<RttEstimator> rtt() const { return conn_ ? std::optional(conn_->rtt()) : std::nullopt; }
  // The server's clock and the round trip, if there is a connection. See ClockSync.
  std::optional<ClockSync> clock() const { return conn_ ? std::optional(conn_->clock()) : std::nullopt; }
  // Messages received, also after the connection closed, until taken.
  std::optional<Message> poll_message();

  State state() const { return state_; }
  ConnectionId connection_id() const { return id_; }
  DisconnectReason close_reason() const { return close_reason_; }
  // Round trip of the first Init that was answered without a resend, if there was one.
  std::optional<Micros> handshake_rtt() const { return handshake_rtt_; }
  // The server's clock as told in its Accept, and when (our clock) the Accept arrived.
  std::uint64_t accept_server_time() const { return accept_server_time_; }
  Micros accept_local_time() const { return accept_local_time_; }

 private:
  void handle(const Address& from, std::span<const std::uint8_t> datagram);
  void send_current(Micros now);

  Link& link_;
  Address server_;
  ClientConfig cfg_;
  State state_ = State::Idle;
  std::uint64_t nonce_ = 0;
  std::uint64_t base_seed_;
  std::uint64_t attempts_ = 0;
  bool have_challenge_ = false;
  ChallengeBody challenge_;
  Micros started_ = 0, last_sent_ = 0, now_ = 0;
  bool resent_init_ = false;
  ConnectionId id_ = 0;
  DisconnectReason close_reason_ = DisconnectReason::Closed;
  std::optional<Micros> handshake_rtt_;
  std::uint64_t accept_server_time_ = 0;
  Micros accept_local_time_ = 0;
  std::optional<Connection> conn_;
  std::array<std::uint8_t, kMaxDatagram + 1> rx_{};
};

}  // namespace npk::transport
