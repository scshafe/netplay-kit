#include "npk/session/server_session.hpp"

namespace npk::session {

namespace {
MatchConfig with_transport_bounds(const ServerSessionConfig& c) {
  MatchConfig m = c.match;
  m.max_message_size = c.transport.connection.max_message_size;
  return m;
}
}  // namespace

ServerSession::ServerSession(transport::Link& link, Simulation& sim, const ServerSessionConfig& config)
    : server_(link, config.transport), match_(*this, sim, with_transport_bounds(config)) {}

std::optional<transport::RttEstimator> ServerSession::rtt(std::size_t seat) const {
  const auto id = match_.connection(seat);
  return id ? server_.rtt(*id) : std::nullopt;
}

void ServerSession::update(transport::Micros now) {
  server_.update(now);
  while (auto e = server_.poll_event()) {
    if (e->type == transport::ServerEvent::Type::Connected) {
      if (!match_.add_player(e->id)) server_.disconnect(e->id, transport::DisconnectReason::Closed);  // no seat
    } else {
      match_.remove_player(e->id);
    }
  }
  while (auto m = server_.poll_message()) match_.on_message(m->id, m->message);
  match_.update(now);
}

}  // namespace npk::session
