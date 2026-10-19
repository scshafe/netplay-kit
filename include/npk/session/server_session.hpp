#pragma once

// A server that hosts exactly one match: a transport Server, and a Match that
// takes the first `player_count` clients to connect, in order of arrival, as its
// players. This is the simple case (tests, small games and tools); the
// GameServer (npk/gameserver) hosts many matches with lobby-issued tokens.

#include "npk/session/match.hpp"
#include "npk/transport/server.hpp"

namespace npk::session {

struct ServerSessionConfig {
  transport::ServerConfig transport{};
  MatchConfig match{};
};

using ServerSessionStats = MatchStats;

class ServerSession final : private MatchIo {
 public:
  using State = Match::State;

  ServerSession(transport::Link& link, Simulation& sim, const ServerSessionConfig& config = {});

  void update(transport::Micros now);

  State state() const { return match_.state(); }
  Frame next_frame() const { return match_.next_frame(); }
  transport::Micros start_time() const { return match_.start_time(); }
  std::size_t seated() const { return match_.seated(); }
  bool seat_connected(std::size_t seat) const { return match_.seat_connected(seat); }
  std::optional<transport::RttEstimator> rtt(std::size_t seat) const;
  const ServerSessionStats& stats() const { return match_.stats(); }
  const Match& match() const { return match_; }
  const transport::Server& transport() const { return server_; }

 private:
  bool send_unreliable(transport::ConnectionId id, std::span<const std::uint8_t> d) override {
    return server_.send_unreliable(id, d);
  }
  bool send_reliable(transport::ConnectionId id, std::span<const std::uint8_t> d) override {
    return server_.send_reliable(id, d);
  }
  void disconnect(transport::ConnectionId id, transport::DisconnectReason why) override { server_.disconnect(id, why); }

  transport::Server server_;
  Match match_;
};

}  // namespace npk::session
