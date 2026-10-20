#pragma once

// The game server: one long-running process that hosts many matches
// (docs/decisions/0003-room-handoff.md).
//
//  - UDP: one port for all matches. A client's first message is a `Join` (room id
//    and the 16-byte token the lobby issued for its seat). A connection that does
//    not join in time, or joins wrongly too often, is closed.
//  - Control API (HTTP/JSON, shared secret as `Authorization: Bearer ...`), for
//    the lobby only:
//      POST   /rooms        {"room_id", "seats": [{"token": <32 hex>}...], "options": {...} | "..."}
//                           201 created; 409 {"error":"full"|"exists"}; 400 {"error":...}
//      GET    /rooms/<id>   {"room_id", "seats", "options"} (never the tokens), or 404
//      DELETE /rooms/<id>   204, or 404; ends the match without a result
//      GET    /status       {"max_matches", "matches", "rooms": [...]}
//  - To the lobby (when `lobby` is set): `POST /control/servers` every few seconds
//    (control and UDP addresses, capacity, load), and `POST /control/rooms/<id>/result`
//    when a match ends, retried for a while if the lobby does not answer.
//
// The game never appears here: `factory` makes a Simulation for a room of N
// seats from the `options` the lobby passed on. Tokens and the secret are never
// logged.

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "npk/http/client.hpp"
#include "npk/http/server.hpp"
#include "npk/json/json.hpp"
#include "npk/session/match.hpp"
#include "npk/transport/server.hpp"

namespace npk::gameserver {

// Returns null if it cannot make a game for these seats and options.
using SimulationFactory =
    std::function<std::unique_ptr<session::Simulation>(std::size_t players, const json::Value& options)>;

struct GameServerConfig {
  SimulationFactory factory;
  std::string control_secret;  // required, at least kMinSecret bytes
  transport::Address control_bind = transport::Address::any_v4(9000);
  std::size_t max_matches = 16;
  transport::ServerConfig transport{};
  session::MatchConfig match{};
  http::ServerConfig control_http{};

  // Registration with the lobby. All empty: no lobby, rooms only come from the control API.
  std::string lobby;              // "host:port" or "http://host:port"
  std::string advertise_control;  // how the lobby reaches the control API, "host:port"
  std::string advertise_udp;      // how players reach the UDP port, "host:port"
  transport::Micros register_interval = 5'000'000;

  transport::Micros unjoined_timeout = 5'000'000;
  std::size_t max_unjoined = 64;
  std::size_t max_join_failures = 3;
  std::size_t max_result_tries = 12;
  transport::Micros result_retry = 5'000'000;
  std::function<void(std::string_view)> log;  // optional; never given a token or the secret
};

inline constexpr std::size_t kMinSecret = 16;
inline constexpr std::size_t kMaxSeats = 16;

struct GameServerStats {
  std::uint64_t matches_started = 0;
  std::uint64_t matches_finished = 0;
  std::uint64_t joins_ok = 0;
  std::uint64_t joins_failed = 0;
  std::uint64_t unjoined_timeouts = 0;
  std::uint64_t unauthorized = 0;       // control requests without the secret
  std::uint64_t rooms_refused = 0;      // full, duplicate or invalid
  std::uint64_t registrations_ok = 0;
  std::uint64_t registrations_failed = 0;
  std::uint64_t results_sent = 0;
  std::uint64_t results_dropped = 0;    // given up on after max_result_tries
};

class GameServer final : private session::MatchIo {
 public:
  // Null on failure (no factory, a short secret, the control port is taken), with the reason in *error.
  static std::unique_ptr<GameServer> create(transport::Link& link, GameServerConfig config,
                                            std::string* error = nullptr);
  ~GameServer() override;

  void update(transport::Micros now);

  transport::Address control_address() const { return control_->local_address(); }
  std::size_t match_count() const { return rooms_.size(); }
  const GameServerStats& stats() const { return stats_; }
  const transport::Server& transport() const { return server_; }

 private:
  struct Room {
    std::string id;
    json::Value options;
    std::unique_ptr<session::Simulation> sim;
    std::unique_ptr<session::Match> match;
    std::vector<std::array<std::uint8_t, session::msg::kTokenSize>> tokens;
  };
  struct Unjoined {
    transport::Micros since;
    std::size_t failures = 0;
  };
  struct PendingResult {
    std::string room;
    std::string body;
    std::size_t tries = 0;
    transport::Micros next_try = 0;
    bool in_flight = false;
  };

  GameServer(transport::Link& link, GameServerConfig config);
  bool send_unreliable(transport::ConnectionId id, std::span<const std::uint8_t> d) override;
  bool send_reliable(transport::ConnectionId id, std::span<const std::uint8_t> d) override;
  void disconnect(transport::ConnectionId id, transport::DisconnectReason why) override;

  void on_control(const http::Request& r, http::Responder rs);
  http::Response create_room(const http::Request& r);
  http::Response room_view(std::string_view id) const;
  http::Response cancel_room(std::string_view id);
  http::Response status() const;
  void handle_join(transport::ConnectionId id, const transport::Message& m, transport::Micros now);
  void join_failed(transport::ConnectionId id);
  void end_room(const std::string& id, bool report);
  void queue_result(Room& room);
  void pump_lobby(transport::Micros now);
  void say(std::string_view s) const {
    if (cfg_.log) cfg_.log(s);
  }

  transport::Server server_;
  GameServerConfig cfg_;
  std::unique_ptr<http::Server> control_;
  http::Client client_;
  std::unordered_map<std::string, Room> rooms_;
  std::unordered_map<transport::ConnectionId, std::string> in_room_;
  std::unordered_map<transport::ConnectionId, Unjoined> unjoined_;
  std::vector<PendingResult> results_;
  std::optional<transport::Address> lobby_addr_;
  std::string lobby_host_;
  transport::Micros next_register_ = 0, next_resolve_ = 0, now_ = 0;
  bool register_in_flight_ = false;
  int registered_ = -1;  // -1 not tried yet, 0 failing, 1 working: for logging changes only
  GameServerStats stats_;
};

}  // namespace npk::gameserver
