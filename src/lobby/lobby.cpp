#include "npk/lobby/lobby.hpp"

#include <algorithm>
#include <charconv>

#include "npk/session/messages.hpp"
#include "npk/util/random.hpp"

namespace npk::lobby {

using http::Request;
using http::Response;
using transport::Micros;

namespace {

Response error(int status, std::string_view what) {
  json::Value v;
  v.set("error", std::string(what));
  return Response::json(status, json::dump(v));
}

Response ok_json(const json::Value& v, int status = 200) { return Response::json(status, json::dump(v)); }

std::vector<std::string> split_path(const std::string& path) {
  std::vector<std::string> seg;
  std::size_t i = 1;  // skip the leading '/'
  while (i <= path.size()) {
    const std::size_t j = path.find('/', i);
    seg.push_back(path.substr(i, j == std::string::npos ? std::string::npos : j - i));
    if (j == std::string::npos) break;
    i = j + 1;
  }
  return seg;
}

// Value of `key` in a query string like "a=1&b=2", only if it is all digits (and short).
std::optional<std::uint64_t> query_number(const std::string& q, std::string_view key) {
  std::size_t i = 0;
  while (i < q.size()) {
    const std::size_t amp = q.find('&', i);
    const std::string_view part = std::string_view(q).substr(i, amp == std::string::npos ? std::string::npos : amp - i);
    const std::size_t eq = part.find('=');
    if (eq != std::string_view::npos && part.substr(0, eq) == key) {
      const std::string_view v = part.substr(eq + 1);
      std::uint64_t n = 0;
      if (v.empty() || v.size() > 12) return std::nullopt;
      auto r = std::from_chars(v.data(), v.data() + v.size(), n);
      if (r.ec != std::errc() || r.ptr != v.data() + v.size()) return std::nullopt;
      return n;
    }
    if (amp == std::string::npos) break;
    i = amp + 1;
  }
  return std::nullopt;
}

std::optional<json::Value> object_body(const Request& r) {
  json::Limits l;
  l.max_bytes = 16 * 1024;
  auto v = json::parse(r.body.empty() ? "{}" : r.body, l);
  if (!v || !v->is_object()) return std::nullopt;
  return v;
}

bool valid_label(std::string_view s, std::size_t max) {  // printable text, no control characters
  if (s.empty() || s.size() > max) return false;
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) return false;
  }
  return s.front() != ' ' && s.back() != ' ';
}

bool valid_game(std::string_view s) {
  if (s.empty() || s.size() > 24) return false;
  for (const char c : s) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return true;
}

const char* state_name(int s) {
  static const char* names[] = {"open", "starting", "playing", "finished"};
  return names[s];
}

}  // namespace

Lobby::Lobby(const LobbyConfig& c)
    : cfg_(c),
      client_(http::ClientConfig{.timeout = c.assign_timeout}),
      writes_(1024, c.writes_per_second, c.write_burst) {
  util::random_bytes(rate_key_);
}

Lobby::~Lobby() = default;

std::unique_ptr<Lobby> Lobby::create(const LobbyConfig& config, std::string* error) {
  if (config.control_secret.size() < kMinSecret) {
    if (error) *error = "the control secret is missing or too short";
    return nullptr;
  }
  std::unique_ptr<Lobby> l(new Lobby(config));
  Lobby* self = l.get();
  std::string why;
  l->http_ = http::Server::listen(
      config.bind,
      [self](const Request& r, const transport::Address& peer, http::Responder rs) { self->handle(r, peer, rs); },
      config.http, &why);
  if (!l->http_) {
    if (error) *error = "http: " + why;
    return nullptr;
  }
  return l;
}

std::string Lobby::new_id() {
  for (;;) {
    std::string id = util::random_id(8);
    if (!rooms_.count(id)) return id;
  }
}

// ---- routing ----

void Lobby::handle(const Request& r, const transport::Address& peer, http::Responder rs) {
  const auto seg = split_path(r.path);
  Response out;
  bool deferred = false;
  if (!seg.empty() && seg[0] == "api") {
    out = route_api(r, seg, rs, peer, deferred);
  } else if (!seg.empty() && seg[0] == "control") {
    out = http::constant_time_equal(http::bearer_token(r), cfg_.control_secret)
              ? route_control(r, seg)
              : (++stats_.unauthorized, error(401, "unauthorized"));
  } else if (r.method == "GET" || r.method == "HEAD") {
    out = serve_static(r);
  } else {
    out = error(405, "method not allowed");
  }
  if (!deferred) rs.send(std::move(out));
}

Response Lobby::serve_static(const Request& r) const {
  const Asset* embedded = default_asset(r.path);
  std::optional<Asset> from_dir;
  if (!cfg_.site_dir.empty()) from_dir = read_site_file(cfg_.site_dir, r.path);
  if (!from_dir && !embedded) return error(404, "not found");
  const std::string& type = from_dir ? from_dir->content_type : embedded->content_type;
  Response out;
  out.status = 200;
  out.content_type = type;
  out.body = from_dir ? from_dir->body : embedded->body;
  // Nothing but the lobby's own files may load, run or be embedded.
  out.headers.emplace_back("Content-Security-Policy",
                           "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; font-src 'self'; "
                           "connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
  out.headers.emplace_back("Referrer-Policy", "no-referrer");
  out.headers.emplace_back("X-Frame-Options", "DENY");
  return out;
}

Response Lobby::route_api(const Request& r, const std::vector<std::string>& seg, http::Responder rs,
                          const transport::Address& peer, bool& deferred) {
  if (r.method != "GET" && r.method != "HEAD") {
    std::uint64_t h = transport::siphash24(rate_key_, std::span<const std::uint8_t>(peer.ip));
    if (!writes_.take(h, now_)) {
      ++stats_.rate_limited;
      Response limited = error(429, "slow down");
      limited.headers.emplace_back("Retry-After", "2");
      return limited;
    }
  }
  // /api/status, /api/rooms, /api/rooms/<id>[/<action>]
  if (seg.size() == 2 && seg[1] == "status" && r.method == "GET") {
    json::Value v;
    v.set("rooms", static_cast<std::int64_t>(rooms_.size()));
    std::int64_t live = 0;
    for (const auto& kv : servers_) live += now_ - kv.second.seen <= cfg_.server_ttl;
    v.set("game_servers", live);
    return ok_json(v);
  }
  if (seg.size() == 2 && seg[1] == "site" && r.method == "GET") return ok_json(cfg_.site.to_json());
  if (seg.size() < 2 || seg[1] != "rooms") return error(404, "not found");
  if (seg.size() == 2) {
    if (r.method == "GET") {
      json::Value list = json::Value(json::Value::Array{});
      std::vector<const Room*> rooms;
      for (const auto& kv : rooms_) rooms.push_back(&kv.second);
      std::sort(rooms.begin(), rooms.end(), [](const Room* a, const Room* b) { return a->created > b->created; });
      for (const Room* room : rooms) list.push_back(summary(*room));
      json::Value v;
      v.set("rooms", list);
      return ok_json(v);
    }
    if (r.method == "POST") return create_room(r);
    return error(405, "method not allowed");
  }
  const auto it = session::msg::valid_room_id(seg[2]) ? rooms_.find(seg[2]) : rooms_.end();
  if (it == rooms_.end()) return error(404, "no such room");
  Room& room = it->second;
  if (seg.size() == 3) {
    if (r.method != "GET") return error(405, "method not allowed");
    if (Seat* s = seat_for(room, r)) s->seen = now_;  // a poll with the seat's secret shows it is still here
    const auto since = query_number(r.query, "since");
    auto wait = query_number(r.query, "wait");
    if (since && *since == room.version && wait && *wait > 0 && waiters_ < cfg_.max_waiters && room.waiters.size() < 64 &&
        room.state != State::Finished) {
      const Micros hold = std::min<Micros>(static_cast<Micros>(*wait) * 1'000'000, cfg_.max_wait);
      room.waiters.push_back(Waiter{rs, *since, now_ + hold});
      ++waiters_;
      deferred = true;
      return {};
    }
    return ok_json(room_view(room));
  }
  if (seg.size() != 4) return error(404, "not found");
  const std::string& action = seg[3];
  if (r.method != "POST") return error(405, "method not allowed");
  if (action == "join") return join(room, r);
  Seat* seat = seat_for(room, r);
  if (action == "ready") return set_ready(room, seat, r);
  if (action == "leave") return leave(room, seat);
  if (action == "me") return me(room, seat);
  return error(404, "not found");
}

Response Lobby::route_control(const Request& r, const std::vector<std::string>& seg) {
  if (seg.size() == 2 && seg[1] == "servers") return r.method == "POST" ? register_server(r) : error(405, "method not allowed");
  if (seg.size() == 4 && seg[1] == "rooms" && seg[3] == "result") {
    if (r.method != "POST") return error(405, "method not allowed");
    const auto it = rooms_.find(seg[2]);
    if (it == rooms_.end()) return error(404, "no such room");
    return room_result(it->second, r);
  }
  return error(404, "not found");
}

// ---- views ----

json::Value Lobby::summary(const Room& room) const {
  json::Value v;
  v.set("id", room.id);
  v.set("name", room.name);
  v.set("game", room.game);
  v.set("state", state_name(static_cast<int>(room.state)));
  std::int64_t taken = 0;
  for (const Seat& s : room.seats) taken += s.taken;
  v.set("seat_count", static_cast<std::int64_t>(room.seats.size()));
  v.set("taken", taken);
  v.set("version", static_cast<std::int64_t>(room.version));
  return v;
}

json::Value Lobby::room_view(const Room& room) const {
  json::Value v = summary(room);
  json::Value seats = json::Value(json::Value::Array{});
  for (std::size_t i = 0; i < room.seats.size(); ++i) {
    json::Value s;
    s.set("index", static_cast<std::int64_t>(i));
    s.set("name", room.seats[i].taken ? json::Value(room.seats[i].name) : json::Value());
    s.set("ready", room.seats[i].ready);
    seats.push_back(s);
  }
  v.set("seats", seats);
  if (!room.error.empty()) v.set("error", room.error);
  if (room.state == State::Finished) v.set("result", room.result);
  return v;
}

void Lobby::changed(Room& room) {
  ++room.version;
  room.activity = now_;
  const std::string body = json::dump(room_view(room));
  for (Waiter& w : room.waiters) {
    w.responder.send(Response::json(200, body));
    --waiters_;
  }
  room.waiters.clear();
}

Lobby::Seat* Lobby::seat_for(Room& room, const Request& r) {
  const std::string_view token = http::bearer_token(r);
  if (token.empty()) return nullptr;
  Seat* found = nullptr;
  for (Seat& s : room.seats) {  // every seat, so the time does not tell which one matched
    if (s.taken && http::constant_time_equal(s.secret, token) && !found) found = &s;
  }
  return found;
}

// ---- rooms ----

Response Lobby::create_room(const Request& r) {
  auto body = object_body(r);
  if (!body) return error(400, "body is not a JSON object");
  if (rooms_.size() >= cfg_.max_rooms) return error(503, "too many rooms");
  const json::Value* name = body->find("name");
  if (!name || !name->as_string() || !valid_label(*name->as_string(), 40)) return error(400, "bad name");
  std::string game = "default";
  if (const json::Value* g = body->find("game")) {
    if (!g->as_string() || !valid_game(*g->as_string())) return error(400, "bad game");
    game = *g->as_string();
  }
  std::int64_t seats = 2;
  if (const json::Value* s = body->find("seats")) {
    if (!s->as_int()) return error(400, "bad seats");
    seats = *s->as_int();
  }
  if (seats < 1 || seats > static_cast<std::int64_t>(cfg_.max_seats)) return error(400, "bad seats");
  json::Value options = json::Value(json::Value::Object{});
  if (const json::Value* o = body->find("options")) {
    if (!o->is_object() || json::dump(*o).size() > 4096) return error(400, "bad options");
    options = *o;
  }
  Room room;
  room.id = new_id();
  room.name = *name->as_string();
  room.game = game;
  room.seats.resize(static_cast<std::size_t>(seats));
  room.options = std::move(options);
  room.created = room.activity = room.empty_since = now_;
  const std::string id = room.id;
  Room& stored = rooms_.emplace(id, std::move(room)).first->second;
  ++stats_.rooms_created;
  return ok_json(room_view(stored), 201);
}

Response Lobby::join(Room& room, const Request& r) {
  auto body = object_body(r);
  if (!body) return error(400, "body is not a JSON object");
  const json::Value* name = body->find("name");
  if (!name || !name->as_string() || !valid_label(*name->as_string(), 24)) return error(400, "bad name");
  if (room.state != State::Open) return error(409, "started");
  Seat* free_seat = nullptr;
  std::size_t index = 0;
  for (std::size_t i = 0; i < room.seats.size() && !free_seat; ++i) {
    if (!room.seats[i].taken) {
      free_seat = &room.seats[i];
      index = i;
    }
  }
  if (!free_seat) return error(409, "full");
  free_seat->taken = true;
  free_seat->ready = false;
  free_seat->name = *name->as_string();
  free_seat->secret = util::random_hex(16);
  free_seat->seen = now_;
  const std::string secret = free_seat->secret;
  changed(room);
  json::Value v;
  v.set("seat", static_cast<std::int64_t>(index));
  v.set("secret", secret);
  v.set("room", room_view(room));
  return ok_json(v);
}

Response Lobby::set_ready(Room& room, Seat* seat, const Request& r) {
  if (!seat) return error(401, "no seat");
  auto body = object_body(r);
  const json::Value* ready = body ? body->find("ready") : nullptr;
  if (!ready || !ready->as_bool()) return error(400, "bad ready");
  if (room.state != State::Open) return error(409, "started");
  seat->seen = now_;
  seat->ready = *ready->as_bool();
  room.error.clear();
  changed(room);
  maybe_start(room);
  return ok_json(room_view(room));
}

Response Lobby::leave(Room& room, Seat* seat) {
  if (!seat) return error(401, "no seat");
  if (room.state == State::Open) {
    *seat = Seat{};
    if (std::none_of(room.seats.begin(), room.seats.end(), [](const Seat& s) { return s.taken; })) room.empty_since = now_;
    changed(room);
  } else if (room.state != State::Finished) {
    return error(409, "started");
  }
  return Response::empty(204);
}

Response Lobby::me(Room& room, Seat* seat) {
  if (!seat) return error(401, "no seat");
  seat->seen = now_;
  json::Value v;
  v.set("state", state_name(static_cast<int>(room.state)));
  if (room.state == State::Playing) {
    const auto it = servers_.find(room.server);
    v.set("room_id", room.id);
    v.set("token", seat->token);
    v.set("seat", static_cast<std::int64_t>(seat - room.seats.data()));
    json::Value server;
    server.set("udp", it == servers_.end() ? std::string() : it->second.udp);
    v.set("server", server);
  }
  return ok_json(v);
}

// ---- starting a room ----

void Lobby::maybe_start(Room& room) {
  if (room.state != State::Open) return;
  for (const Seat& s : room.seats) {
    if (!s.taken || !s.ready) return;
  }
  room.state = State::Starting;
  json::Value seats = json::Value(json::Value::Array{});
  for (Seat& s : room.seats) {
    s.token = util::random_hex(session::msg::kTokenSize);
    json::Value one;
    one.set("token", s.token);
    seats.push_back(one);
  }
  json::Value body;
  body.set("room_id", room.id);
  body.set("seats", seats);
  body.set("options", room.options);
  changed(room);
  assign(room, json::dump(body), {});
}

Lobby::GameServer* Lobby::pick_server(const std::vector<std::string>& tried) {
  GameServer* best = nullptr;
  std::int64_t best_free = 0;
  const std::string* best_key = nullptr;
  for (auto& kv : servers_) {
    GameServer& g = kv.second;
    if (now_ - g.seen > cfg_.server_ttl || now_ < g.avoid_until) continue;
    if (std::find(tried.begin(), tried.end(), kv.first) != tried.end()) continue;
    const std::int64_t free = g.max_matches - std::max(g.matches, g.assigned);
    if (free <= 0) continue;
    if (!best || free > best_free || (free == best_free && kv.first < *best_key)) {
      best = &g;
      best_free = free;
      best_key = &kv.first;
    }
  }
  return best;
}

void Lobby::assign(Room& room, std::string body, std::vector<std::string> tried) {
  GameServer* g = pick_server(tried);
  if (!g) return fail_assignment(room, "no_server");
  const std::string key = g->control;
  tried.push_back(key);
  const auto hp = http::parse_host_port(g->control, 80);
  if (!g->addr || now_ - g->resolved > 60'000'000) {
    g->addr = hp ? http::resolve(hp->host, hp->port) : std::nullopt;
    g->resolved = now_;
  }
  if (!g->addr) {
    g->avoid_until = now_ + cfg_.server_penalty;
    return assign(room, std::move(body), std::move(tried));
  }
  http::OutgoingRequest q;
  q.method = "POST";
  q.path = "/rooms";
  q.body = body;
  q.headers.emplace_back("Authorization", "Bearer " + cfg_.control_secret);
  const std::string id = room.id;
  const bool started = client_.request(*g->addr, g->control, q, now_, [this, id, key, body, tried](const http::Result& res) {
    const auto rit = rooms_.find(id);
    const auto sit = servers_.find(key);
    if (rit == rooms_.end() || rit->second.state != State::Starting) return;
    Room& rm = rit->second;
    if (res.ok && res.status == 201) {
      rm.state = State::Playing;
      rm.server = key;
      if (sit != servers_.end()) ++sit->second.assigned;
      ++stats_.rooms_started;
      say("room " + id + " playing on " + key);
      changed(rm);
      return;
    }
    if (sit != servers_.end()) sit->second.avoid_until = now_ + cfg_.server_penalty;
    assign(rm, body, tried);
  });
  if (!started) fail_assignment(room, "busy");
}

void Lobby::fail_assignment(Room& room, std::string_view why) {
  ++stats_.assignments_failed;
  room.state = State::Open;
  room.error = std::string(why);
  for (Seat& s : room.seats) {
    s.ready = false;
    s.token.clear();
  }
  changed(room);
}

// ---- the game servers' side ----

Response Lobby::register_server(const Request& r) {
  auto body = object_body(r);
  if (!body) return error(400, "body is not a JSON object");
  const json::Value* control = body->find("control");
  const json::Value* udp = body->find("udp");
  const json::Value* max = body->find("max_matches");
  const json::Value* matches = body->find("matches");
  if (!control || !control->as_string() || !http::parse_host_port(*control->as_string(), 0) ||
      control->as_string()->size() > 255)
    return error(400, "bad control address");
  if (!udp || !udp->as_string() || !valid_label(*udp->as_string(), 255)) return error(400, "bad udp address");
  if (!max || !max->as_int() || *max->as_int() < 1 || *max->as_int() > 100000) return error(400, "bad max_matches");
  if (!matches || !matches->as_int() || *matches->as_int() < 0) return error(400, "bad matches");
  const std::string key = *control->as_string();
  if (!servers_.count(key) && servers_.size() >= 64) return error(503, "too many game servers");
  GameServer& g = servers_[key];
  if (g.control.empty()) ++stats_.servers_registered;
  g.control = key;
  g.udp = *udp->as_string();
  g.max_matches = *max->as_int();
  g.matches = *matches->as_int();
  g.seen = now_;
  return ok_json(json::Value(json::Value::Object{}));
}

Response Lobby::room_result(Room& room, const Request& r) {
  if (room.state != State::Playing) return error(409, "not playing");
  auto body = object_body(r);
  if (!body) return error(400, "body is not a JSON object");
  const json::Value* outcome = body->find("outcome");
  if (!outcome || !outcome->as_string() || !valid_label(*outcome->as_string(), 32)) return error(400, "bad outcome");
  json::Value result;
  result.set("outcome", *outcome->as_string());
  if (const json::Value* f = body->find("frames")) result.set("frames", *f);
  if (const json::Value* j = body->find("joined")) result.set("joined", *j);
  if (const json::Value* g = body->find("result")) result.set("game", *g);
  room.result = std::move(result);
  room.state = State::Finished;
  room.finished_at = now_;
  if (const auto sit = servers_.find(room.server); sit != servers_.end() && sit->second.assigned > 0) --sit->second.assigned;
  for (Seat& s : room.seats) s.token.clear();
  ++stats_.results;
  changed(room);
  return ok_json(json::Value(json::Value::Object{}));
}

// ---- time ----

void Lobby::remove_room(const std::string& id) {
  const auto it = rooms_.find(id);
  if (it == rooms_.end()) return;
  for (Waiter& w : it->second.waiters) {
    w.responder.send(error(404, "no such room"));
    --waiters_;
  }
  rooms_.erase(it);
}

void Lobby::update(Micros now) {
  now_ = now;
  http_->update(now);
  client_.update(now);

  std::vector<std::string> gone;
  for (auto& kv : rooms_) {
    Room& room = kv.second;
    // Long-polls that ran out: answer with the room as it is.
    for (std::size_t i = 0; i < room.waiters.size();) {
      Waiter& w = room.waiters[i];
      if (now >= w.deadline || !w.responder.alive()) {
        w.responder.send(ok_json(room_view(room)));
        --waiters_;
        room.waiters.erase(room.waiters.begin() + static_cast<std::ptrdiff_t>(i));
      } else {
        ++i;
      }
    }
    if (room.state == State::Open) {
      bool freed = false;
      for (Seat& s : room.seats) {
        if (s.taken && now - s.seen > cfg_.seat_presence) {
          s = Seat{};
          freed = true;
        }
      }
      if (freed) {
        if (std::none_of(room.seats.begin(), room.seats.end(), [](const Seat& s) { return s.taken; })) room.empty_since = now;
        changed(room);
      }
      const bool empty = std::none_of(room.seats.begin(), room.seats.end(), [](const Seat& s) { return s.taken; });
      if ((empty && now - room.empty_since > cfg_.empty_room_ttl) || now - room.activity > cfg_.idle_room_ttl) gone.push_back(kv.first);
    } else if (room.state == State::Playing) {
      const auto sit = servers_.find(room.server);
      if (sit == servers_.end() || now - sit->second.seen > cfg_.server_ttl) {
        room.result = json::Value(json::Value::Object{});
        room.result.set("outcome", "server_lost");
        room.state = State::Finished;
        room.finished_at = now;
        for (Seat& s : room.seats) s.token.clear();
        changed(room);
      }
    } else if (room.state == State::Finished && now - room.finished_at > cfg_.finished_ttl) {
      gone.push_back(kv.first);
    }
  }
  for (const auto& id : gone) remove_room(id);
  for (auto it = servers_.begin(); it != servers_.end();) {
    it = (now - it->second.seen > 3 * cfg_.server_ttl) ? servers_.erase(it) : std::next(it);
  }
}

}  // namespace npk::lobby
