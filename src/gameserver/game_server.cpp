#include "npk/gameserver/game_server.hpp"

#include <algorithm>

namespace npk::gameserver {

using http::Response;
using transport::ConnectionId;
using transport::Micros;

namespace {

Response error(int status, std::string_view what) {
  json::Value v;
  v.set("error", std::string(what));
  return Response::json(status, json::dump(v));
}

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parse_token(std::string_view hex, std::array<std::uint8_t, session::msg::kTokenSize>& out) {
  if (hex.size() != 2 * out.size()) return false;
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int hi = hex_digit(hex[2 * i]), lo = hex_digit(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
  }
  return true;
}

const char* outcome_name(session::Match::End e) {
  switch (e) {
    case session::Match::End::Completed: return "completed";
    case session::Match::End::Abandoned: return "abandoned";
    case session::Match::End::NoShow: return "no_show";
    case session::Match::End::TimedOut: return "timed_out";
    default: return "unknown";
  }
}

std::string_view as_view(const std::array<std::uint8_t, session::msg::kTokenSize>& t) {
  return {reinterpret_cast<const char*>(t.data()), t.size()};
}

}  // namespace

GameServer::GameServer(transport::Link& link, GameServerConfig config)
    : server_(link, config.transport), cfg_(std::move(config)), client_() {}

GameServer::~GameServer() = default;

std::unique_ptr<GameServer> GameServer::create(transport::Link& link, GameServerConfig config, std::string* error) {
  auto fail = [&](const char* why) -> std::unique_ptr<GameServer> {
    if (error) *error = why;
    return nullptr;
  };
  if (!config.factory) return fail("no simulation factory");
  if (config.control_secret.size() < kMinSecret) return fail("the control secret is missing or too short");
  if (config.max_matches == 0) return fail("max_matches must be at least 1");
  config.match.max_message_size = config.transport.connection.max_message_size;
  std::unique_ptr<GameServer> gs(new GameServer(link, std::move(config)));
  GameServer* self = gs.get();
  std::string why;
  gs->control_ = http::Server::listen(
      self->cfg_.control_bind, [self](const http::Request& r, const transport::Address&, http::Responder rs) {
        self->on_control(r, rs);
      },
      self->cfg_.control_http, &why);
  if (!gs->control_) {
    if (error) *error = "control port: " + why;
    return nullptr;
  }
  if (!self->cfg_.lobby.empty()) {
    auto hp = http::parse_host_port(self->cfg_.lobby, 80);
    if (!hp) return fail("lobby address is not host:port");
    self->lobby_host_ = hp->host + ":" + std::to_string(hp->port);
    self->lobby_addr_ = http::resolve(hp->host, hp->port);
  }
  return gs;
}

bool GameServer::send_unreliable(ConnectionId id, std::span<const std::uint8_t> d) { return server_.send_unreliable(id, d); }
bool GameServer::send_reliable(ConnectionId id, std::span<const std::uint8_t> d) { return server_.send_reliable(id, d); }
void GameServer::disconnect(ConnectionId id, transport::DisconnectReason why) { server_.disconnect(id, why); }

// ---- the control API ----

void GameServer::on_control(const http::Request& r, http::Responder rs) {
  if (!http::constant_time_equal(http::bearer_token(r), cfg_.control_secret)) {
    ++stats_.unauthorized;
    rs.send(error(401, "unauthorized"));
    return;
  }
  Response out;
  if (r.path == "/rooms") {
    out = r.method == "POST" ? create_room(r) : error(405, "method not allowed");
  } else if (r.path.rfind("/rooms/", 0) == 0) {
    out = r.method == "DELETE" ? cancel_room(std::string_view(r.path).substr(7)) : error(405, "method not allowed");
  } else if (r.path == "/status") {
    out = r.method == "GET" ? status() : error(405, "method not allowed");
  } else {
    out = error(404, "not found");
  }
  rs.send(std::move(out));
}

Response GameServer::status() const {
  json::Value v;
  v.set("max_matches", static_cast<std::int64_t>(cfg_.max_matches));
  v.set("matches", static_cast<std::int64_t>(rooms_.size()));
  json::Value ids = json::Value(json::Value::Array{});
  for (const auto& kv : rooms_) ids.push_back(kv.first);
  v.set("rooms", ids);
  return Response::json(200, json::dump(v));
}

Response GameServer::create_room(const http::Request& r) {
  auto refuse = [&](int status, std::string_view why) {
    ++stats_.rooms_refused;
    return error(status, why);
  };
  auto doc = json::parse(r.body);
  if (!doc || !doc->is_object()) return refuse(400, "body is not a JSON object");
  const json::Value* id = doc->find("room_id");
  const json::Value* seats = doc->find("seats");
  if (!id || !id->as_string() || !session::msg::valid_room_id(*id->as_string())) return refuse(400, "bad room_id");
  if (!seats || !seats->as_array() || seats->as_array()->empty() || seats->as_array()->size() > kMaxSeats)
    return refuse(400, "bad seats");
  Room room;
  room.id = *id->as_string();
  for (const json::Value& s : *seats->as_array()) {
    const json::Value* t = s.find("token");
    std::array<std::uint8_t, session::msg::kTokenSize> tok{};
    if (!t || !t->as_string() || !parse_token(*t->as_string(), tok)) return refuse(400, "bad token");
    for (const auto& other : room.tokens) {
      if (other == tok) return refuse(400, "duplicate token");
    }
    room.tokens.push_back(tok);
  }
  if (rooms_.count(room.id)) return refuse(409, "exists");
  if (rooms_.size() >= cfg_.max_matches) return refuse(409, "full");
  json::Value options = json::Value(json::Value::Object{});
  if (const json::Value* o = doc->find("options")) {
    if (!o->is_object()) return refuse(400, "options is not an object");
    options = *o;
  }
  room.sim = cfg_.factory(room.tokens.size(), options);
  if (!room.sim || room.sim->player_count() != room.tokens.size() ||
      !session::msg::shape_supported(room.sim->player_count(), room.sim->input_size()))
    return refuse(400, "unsupported game or options");
  room.match = std::make_unique<session::Match>(static_cast<session::MatchIo&>(*this), *room.sim, cfg_.match);
  const std::string rid = room.id;
  rooms_.emplace(rid, std::move(room));
  ++stats_.matches_started;
  say("room " + rid + " assigned");
  json::Value ok;
  ok.set("room_id", rid);
  return Response::json(201, json::dump(ok));
}

Response GameServer::cancel_room(std::string_view id) {
  const std::string key(id);
  if (!rooms_.count(key)) return error(404, "no such room");
  end_room(key, false);
  return Response::empty(204);
}

// ---- rooms ----

void GameServer::end_room(const std::string& id, bool report) {
  const auto it = rooms_.find(id);
  if (it == rooms_.end()) return;
  Room& room = it->second;
  if (report) queue_result(room);
  for (auto c = in_room_.begin(); c != in_room_.end();) {
    if (c->second == id) {
      server_.disconnect(c->first, transport::DisconnectReason::Closed);
      c = in_room_.erase(c);
    } else {
      ++c;
    }
  }
  rooms_.erase(it);
  ++stats_.matches_finished;
}

void GameServer::queue_result(Room& room) {
  json::Value v;
  v.set("room_id", room.id);
  v.set("outcome", outcome_name(room.match->end()));
  v.set("frames", static_cast<std::int64_t>(room.match->next_frame()));
  json::Value joined = json::Value(json::Value::Array{});
  for (std::size_t i = 0; i < room.match->players(); ++i) joined.push_back(room.match->seat_bound(i));
  v.set("joined", joined);
  // The game's own account, if it is well-formed JSON and small.
  json::Limits small;
  small.max_bytes = 16 * 1024;
  auto game = json::parse(room.sim->result_json(), small);
  v.set("result", game ? std::move(*game) : json::Value());
  PendingResult p;
  p.room = room.id;
  p.body = json::dump(v);
  results_.push_back(std::move(p));
}

// ---- joining ----

void GameServer::join_failed(ConnectionId id) {
  ++stats_.joins_failed;
  const auto it = unjoined_.find(id);
  if (it == unjoined_.end()) return;
  if (++it->second.failures >= cfg_.max_join_failures) {
    server_.disconnect(id, transport::DisconnectReason::ProtocolError);
    unjoined_.erase(it);
  }
}

void GameServer::handle_join(ConnectionId id, const transport::Message& m, Micros) {
  session::msg::Join j;
  if (m.channel != transport::Channel::Reliable || !session::msg::decode_join(m.data, j)) {
    join_failed(id);
    return;
  }
  const auto rit = rooms_.find(j.room);
  // Compare against every seat's token whatever the room, so a wrong guess takes the same time.
  std::size_t found = session::Match::npos;
  if (rit != rooms_.end()) {
    Room& room = rit->second;
    for (std::size_t i = 0; i < room.tokens.size(); ++i) {
      const bool same = http::constant_time_equal(as_view(room.tokens[i]), as_view(j.token));
      if (same && !room.match->seat_connected(i) && found == session::Match::npos) found = i;
    }
  }
  if (found == session::Match::npos || !rit->second.match->add_player(id, found)) {
    join_failed(id);
    return;
  }
  ++stats_.joins_ok;
  unjoined_.erase(id);
  in_room_[id] = j.room;
}

// ---- the loop ----

void GameServer::update(Micros now) {
  now_ = now;
  server_.update(now);
  while (auto e = server_.poll_event()) {
    if (e->type == transport::ServerEvent::Type::Connected) {
      if (unjoined_.size() >= cfg_.max_unjoined) server_.disconnect(e->id, transport::DisconnectReason::Closed);
      else unjoined_[e->id] = Unjoined{now, 0};
    } else {
      unjoined_.erase(e->id);
      if (const auto it = in_room_.find(e->id); it != in_room_.end()) {
        if (const auto r = rooms_.find(it->second); r != rooms_.end()) r->second.match->remove_player(e->id);
        in_room_.erase(it);
      }
    }
  }
  while (auto m = server_.poll_message()) {
    if (const auto it = in_room_.find(m->id); it != in_room_.end()) {
      if (const auto r = rooms_.find(it->second); r != rooms_.end()) r->second.match->on_message(m->id, m->message);
    } else if (unjoined_.count(m->id)) {
      handle_join(m->id, m->message, now);
    }
  }
  for (auto it = unjoined_.begin(); it != unjoined_.end();) {
    if (now - it->second.since >= cfg_.unjoined_timeout) {
      server_.disconnect(it->first, transport::DisconnectReason::Timeout);
      ++stats_.unjoined_timeouts;
      it = unjoined_.erase(it);
    } else {
      ++it;
    }
  }
  std::vector<std::string> done;
  for (auto& kv : rooms_) {
    kv.second.match->update(now);
    if (kv.second.match->done()) done.push_back(kv.first);
  }
  for (const auto& id : done) end_room(id, true);

  control_->update(now);
  client_.update(now);
  pump_lobby(now);
}

void GameServer::pump_lobby(Micros now) {
  if (cfg_.lobby.empty()) return;
  if (now >= next_resolve_ && !lobby_addr_) {
    next_resolve_ = now + 30'000'000;
    if (auto hp = http::parse_host_port(cfg_.lobby, 80)) lobby_addr_ = http::resolve(hp->host, hp->port);
    if (!lobby_addr_ && registered_ != 0) {
      say("cannot find the lobby's address");
      registered_ = 0;
    }
  }
  if (!lobby_addr_) return;
  http::OutgoingRequest base;
  base.headers.emplace_back("Authorization", "Bearer " + cfg_.control_secret);

  if (now >= next_register_ && !register_in_flight_) {
    next_register_ = now + cfg_.register_interval;
    json::Value v;
    v.set("control", cfg_.advertise_control);
    v.set("udp", cfg_.advertise_udp);
    v.set("max_matches", static_cast<std::int64_t>(cfg_.max_matches));
    v.set("matches", static_cast<std::int64_t>(rooms_.size()));
    http::OutgoingRequest q = base;
    q.method = "POST";
    q.path = "/control/servers";
    q.body = json::dump(v);
    register_in_flight_ = true;
    client_.request(*lobby_addr_, lobby_host_, q, now, [this](const http::Result& r) {
      register_in_flight_ = false;
      if (r.ok && r.status / 100 == 2) {
        ++stats_.registrations_ok;
        if (registered_ != 1) say("registered with the lobby");
        registered_ = 1;
      } else {
        ++stats_.registrations_failed;
        if (registered_ != 0) say("cannot register with the lobby: " + (r.ok ? "status " + std::to_string(r.status) : r.error));
        registered_ = 0;
        lobby_addr_.reset();  // look the name up again: the lobby may have moved
        next_resolve_ = 0;
      }
    });
  }
  for (std::size_t i = 0; i < results_.size(); ++i) {
    PendingResult& p = results_[i];
    if (p.in_flight || now < p.next_try) continue;
    if (p.tries >= cfg_.max_result_tries) {
      ++stats_.results_dropped;
      results_.erase(results_.begin() + static_cast<std::ptrdiff_t>(i));
      --i;
      continue;
    }
    ++p.tries;
    p.in_flight = true;
    http::OutgoingRequest q = base;
    q.method = "POST";
    q.path = "/control/rooms/" + p.room + "/result";
    q.body = p.body;
    const std::string room = p.room;
    const bool started = client_.request(*lobby_addr_, lobby_host_, q, now, [this, room](const http::Result& r) {
      for (std::size_t k = 0; k < results_.size(); ++k) {
        if (results_[k].room != room) continue;
        // Delivered, or the lobby does not know the room (there is nothing more to do): done.
        if (r.ok && (r.status / 100 == 2 || r.status == 404)) {
          ++stats_.results_sent;
          results_.erase(results_.begin() + static_cast<std::ptrdiff_t>(k));
        } else {
          results_[k].in_flight = false;
          results_[k].next_try = now_ + cfg_.result_retry;
        }
        return;
      }
    });
    if (!started) {
      p.in_flight = false;
      --p.tries;
      break;
    }
  }
}

}  // namespace npk::gameserver
