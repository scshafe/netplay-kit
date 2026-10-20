#pragma once

// The lobby: rooms, seats, ready and start, over HTTP + JSON, and the hand-off of
// a ready room to a registered game server (docs/decisions/0003-room-handoff.md).
// It is game-agnostic: a room has a free-form game name and an options object
// that are passed on to the game server unread.
//
// Player API (no accounts; a seat is held by a secret returned when joining, sent
// back as `Authorization: Bearer <seat secret>`):
//   GET  /api/status
//   GET  /api/rooms                      list
//   POST /api/rooms                      {"name", "game", "seats", "options"}  -> 201 room
//   GET  /api/rooms/<id>[?since=V&wait=S]  the room; with since=V it waits (up to S <= 25 s)
//                                        until the version is no longer V (long-poll)
//   POST /api/rooms/<id>/join            {"name"} -> {"seat", "secret"}
//   POST /api/rooms/<id>/ready           {"ready": bool}           (seat secret)
//   POST /api/rooms/<id>/leave                                     (seat secret)
//   POST /api/rooms/<id>/me                                        (seat secret)
//        -> while playing: {"server": {"udp"}, "room_id", "token", "seat"}: what a
//           client needs for the game server's Join. Only that seat's own token.
// Game server API (Bearer control secret):
//   POST /control/servers                {"control", "udp", "max_matches", "matches"}
//   POST /control/rooms/<id>/result      {"outcome", "frames", "joined", "result"}
//
// Room states: open -> starting (all seats taken and ready; assigning) -> playing
// -> finished. A room whose assignment finds no game server with room goes back to
// open with "error": "no_server" and the ready flags cleared.

#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "npk/http/client.hpp"
#include "npk/http/server.hpp"
#include "npk/json/json.hpp"
#include "npk/transport/ratelimit.hpp"
#include "npk/transport/siphash.hpp"

namespace npk::lobby {

inline constexpr std::size_t kMinSecret = 16;

struct LobbyConfig {
  transport::Address bind = transport::Address::any_v4(8080);
  std::string control_secret;  // required, at least kMinSecret bytes
  http::ServerConfig http{};
  std::size_t max_rooms = 256;
  std::size_t max_seats = 16;
  std::size_t max_waiters = 512;                  // long-polls held in all
  transport::Micros server_ttl = 20'000'000;      // a game server not heard from this long is gone
  transport::Micros assign_timeout = 5'000'000;
  transport::Micros server_penalty = 10'000'000;  // after a refusal or failure, how long before trying it again
  transport::Micros seat_presence = 45'000'000;   // an open room's seat not seen this long is freed
  transport::Micros empty_room_ttl = 120'000'000;
  transport::Micros idle_room_ttl = 1'800'000'000;
  transport::Micros finished_ttl = 600'000'000;
  transport::Micros max_wait = 25'000'000;
  // Per source address: requests that change something (a token bucket).
  double writes_per_second = 2;
  double write_burst = 20;
  std::function<void(std::string_view)> log;  // optional; never given a secret or a token
};

struct LobbyStats {
  std::uint64_t rooms_created = 0;
  std::uint64_t rooms_started = 0;
  std::uint64_t assignments_failed = 0;
  std::uint64_t rate_limited = 0;
  std::uint64_t unauthorized = 0;
  std::uint64_t servers_registered = 0;
  std::uint64_t results = 0;
};

class Lobby {
 public:
  static std::unique_ptr<Lobby> create(const LobbyConfig& config, std::string* error = nullptr);
  ~Lobby();

  void update(transport::Micros now);
  transport::Address address() const { return http_->local_address(); }
  const LobbyStats& stats() const { return stats_; }
  std::size_t room_count() const { return rooms_.size(); }
  std::size_t server_count() const { return servers_.size(); }

 private:
  struct Seat {
    bool taken = false;
    bool ready = false;
    std::string name;
    std::string secret;
    std::string token;  // hex, issued when the room starts
    transport::Micros seen = 0;
  };
  struct Waiter {
    http::Responder responder;
    std::uint64_t since;
    transport::Micros deadline;
  };
  enum class State { Open, Starting, Playing, Finished };
  struct Room {
    std::string id, name, game;
    State state = State::Open;
    std::vector<Seat> seats;
    json::Value options;
    std::uint64_t version = 1;
    std::string error;
    std::string server;  // key of the game server it was assigned to
    json::Value result;
    transport::Micros created = 0, activity = 0, empty_since = 0, finished_at = 0;
    std::vector<Waiter> waiters;
  };
  struct GameServer {
    std::string control, udp;
    std::int64_t max_matches = 0, matches = 0;
    transport::Micros seen = 0, avoid_until = 0;
    std::optional<transport::Address> addr;
    transport::Micros resolved = 0;
    std::int64_t assigned = 0;  // rooms the lobby has sent there that have not reported back
  };

  explicit Lobby(const LobbyConfig& c);
  void handle(const http::Request& r, const transport::Address& peer, http::Responder rs);
  http::Response route_api(const http::Request& r, const std::vector<std::string>& seg, http::Responder rs,
                           const transport::Address& peer, bool& deferred);
  http::Response route_control(const http::Request& r, const std::vector<std::string>& seg);
  http::Response create_room(const http::Request& r);
  http::Response join(Room& room, const http::Request& r);
  http::Response set_ready(Room& room, Seat* seat, const http::Request& r);
  http::Response leave(Room& room, Seat* seat);
  http::Response me(Room& room, Seat* seat);
  http::Response register_server(const http::Request& r);
  http::Response room_result(Room& room, const http::Request& r);
  Seat* seat_for(Room& room, const http::Request& r);
  json::Value room_view(const Room& room) const;
  json::Value summary(const Room& room) const;
  void changed(Room& room);
  void maybe_start(Room& room);
  void assign(Room& room, std::string body, std::vector<std::string> tried);
  void fail_assignment(Room& room, std::string_view why);
  void remove_room(const std::string& id);
  GameServer* pick_server(const std::vector<std::string>& tried);
  std::string new_id();
  void say(std::string_view s) const {
    if (cfg_.log) cfg_.log(s);
  }

  LobbyConfig cfg_;
  std::unique_ptr<http::Server> http_;
  http::Client client_;
  std::unordered_map<std::string, Room> rooms_;
  std::unordered_map<std::string, GameServer> servers_;
  std::size_t waiters_ = 0;
  transport::SipKey rate_key_{};
  transport::BucketTable writes_;
  transport::Micros now_ = 0;
  LobbyStats stats_;
};

}  // namespace npk::lobby
