// The lobby over real loopback HTTP, with real game servers (simulated UDP for
// the matches) and scripted fake ones for the failure paths. One thread, one
// virtual clock: every component is updated each millisecond.

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>

#include "check.hpp"
#include "npk/gameserver/game_server.hpp"
#include "npk/lobby/lobby.hpp"
#include "npk/session/client_session.hpp"
#include "npk/transport/sim_link.hpp"
#include "toy_game.hpp"

using namespace npk;
using npk::session::Frame;
using npk::transport::Address;
using npk::transport::Micros;

namespace {

constexpr const char* kSecret = "lobby-and-servers-shared-secret";
const Address kUdp = Address::v4(10, 0, 0, 1, 7000);

// A game server that is only an HTTP server answering POST /rooms as scripted.
struct FakeServer {
  explicit FakeServer(int status_) : status(status_) {
    http = http::Server::listen(Address::loopback_v4(0), [this](const http::Request& r, const Address&, http::Responder rs) {
      ++requests;
      if (!http::constant_time_equal(http::bearer_token(r), kSecret)) return rs.send(http::Response::empty(401)), void();
      last_body = r.body;
      rs.send(http::Response::json(status, "{}"));
    });
    CHECK(http);
  }
  std::string control() const { return "127.0.0.1:" + std::to_string(http->local_address().port); }
  std::unique_ptr<http::Server> http;
  int status;
  int requests = 0;
  std::string last_body;
};

http::OutgoingRequest get_request(std::string path) {
  http::OutgoingRequest q;
  q.path = std::move(path);
  return q;
}

struct World {
  explicit World(lobby::LobbyConfig cfg = {}) : net(3) {
    cfg.bind = Address::loopback_v4(0);
    cfg.control_secret = kSecret;
    std::string err;
    lobby = lobby::Lobby::create(cfg, &err);
    CHECK(lobby);
    net.set_default_conditions({.delay = 8'000, .jitter = 2'000});
  }

  void step() {
    const Micros now = net.now();
    lobby->update(now);
    for (auto* f : fakes) f->http->update(now);
    if (gs) gs->update(now);
    caller.update(now);
    for (auto& p : players) p.session->update(now);
    net.advance(1000);
  }
  void run(Micros d) {
    for (Micros t = 0; t < d; t += 1000) step();
  }

  // One request to the lobby (or any server); waits for the answer.
  struct Answer {
    int status = -1;
    json::Value body;
    std::string raw;
    http::Headers headers;
    const std::string* header(std::string_view n) const {
      for (const auto& kv : headers) {
        if (kv.first == n) return &kv.second;
      }
      return nullptr;
    }
  };
  Answer call(const std::string& method, const std::string& path, const std::string& body = "",
              const std::string& bearer = "", Address to = {}) {
    http::OutgoingRequest q;
    q.method = method;
    q.path = path;
    q.body = body;
    if (!bearer.empty()) q.headers.emplace_back("Authorization", "Bearer " + bearer);
    if (to.port == 0) to = lobby->address();
    std::optional<http::Result> got;
    CHECK(caller.request(to, "lobby", q, net.now(), [&](const http::Result& r) { got = r; }));
    for (int i = 0; i < 6000 && !got; ++i) step();
    CHECK(got.has_value());
    Answer a;
    a.status = got->ok ? got->status : -1;
    a.raw = got->body;
    a.headers = got->headers;
    if (auto v = json::parse(got->body)) a.body = std::move(*v);
    return a;
  }
  Answer post(const std::string& path, const std::string& body, const std::string& bearer = "") {
    return call("POST", path, body, bearer);
  }
  Answer control(const std::string& path, const std::string& body) { return call("POST", path, body, kSecret); }

  std::string make_room(const std::string& name = "arena", int seats = 2, const std::string& options = "{}") {
    auto a = post("/api/rooms", "{\"name\":\"" + name + "\",\"seats\":" + std::to_string(seats) + ",\"options\":" + options + "}");
    CHECK(a.status == 201);
    return *a.body.find("id")->as_string();
  }
  struct Seat {
    std::int64_t index;
    std::string secret;
  };
  Seat join(const std::string& room, const std::string& name) {
    auto a = post("/api/rooms/" + room + "/join", "{\"name\":\"" + name + "\"}");
    CHECK(a.status == 200);
    return {*a.body.find("seat")->as_int(), *a.body.find("secret")->as_string()};
  }
  void ready(const std::string& room, const Seat& s, bool v = true) {
    CHECK(post("/api/rooms/" + room + "/ready", std::string("{\"ready\":") + (v ? "true" : "false") + "}", s.secret).status == 200);
  }
  std::string state(const std::string& room) {
    auto a = call("GET", "/api/rooms/" + room);
    CHECK(a.status == 200);
    return *a.body.find("state")->as_string();
  }
  void register_fake(const FakeServer& f, int max = 4, int matches = 0) {
    CHECK(control("/control/servers", "{\"control\":\"" + f.control() + "\",\"udp\":\"fake:7000\",\"max_matches\":" +
                                           std::to_string(max) + ",\"matches\":" + std::to_string(matches) + "}").status == 200);
  }

  struct Player {
    std::unique_ptr<transport::SimLink> link;
    std::unique_ptr<ToyGame> game;
    std::unique_ptr<session::ClientSession> session;
  };
  void start_game_server(gameserver::GameServerConfig cfg = {}) {
    glink = net.make_link(kUdp);
    cfg.factory = [](std::size_t n, const json::Value& o) -> std::unique_ptr<session::Simulation> {
      auto g = std::make_unique<ToyGame>(n);
      if (const auto* e = o.find("end_after"); e && e->as_int()) g->end_after = static_cast<Frame>(*e->as_int());
      return g;
    };
    cfg.control_secret = kSecret;
    cfg.control_bind = Address::loopback_v4(0);
    cfg.transport.seed = 4;
    cfg.lobby = "127.0.0.1:" + std::to_string(lobby->address().port);
    cfg.advertise_control = "127.0.0.1:0";  // replaced below, once the port is known
    cfg.advertise_udp = "game.example:7000";
    cfg.register_interval = 1'000'000;
    // The control port is only known after create(), so register from the test.
    cfg.lobby.clear();
    std::string err;
    gs = gameserver::GameServer::create(*glink, std::move(cfg), &err);
    CHECK(gs);
  }
  void connect_player(const std::string& room, const std::string& token_hex, std::uint8_t host) {
    Player p;
    p.link = net.make_link(Address::v4(10, 1, 0, host, 5000));
    p.game = std::make_unique<ToyGame>(2);
    session::ClientSessionConfig cc;
    cc.transport.seed = host * 11u;
    session::msg::Join j;
    j.room = room;
    CHECK(token_hex.size() == 32);
    for (std::size_t i = 0; i < 16; ++i) j.token[i] = static_cast<std::uint8_t>(std::stoi(token_hex.substr(2 * i, 2), nullptr, 16));
    cc.join = j;
    auto script = std::make_shared<std::vector<std::uint8_t>>(toy_inputs(host, 1, 20000));
    p.session = std::make_unique<session::ClientSession>(
        *p.link, kUdp, *p.game,
        [script](Frame w, std::span<std::uint8_t> o) { o[0] = (*script)[(w % 20000) * 2]; o[1] = (*script)[(w % 20000) * 2 + 1]; }, cc);
    p.session->connect(net.now());
    players.push_back(std::move(p));
  }

  transport::SimNetwork net;
  std::unique_ptr<lobby::Lobby> lobby;
  std::unique_ptr<transport::SimLink> glink;
  std::unique_ptr<gameserver::GameServer> gs;
  std::vector<FakeServer*> fakes;
  http::Client caller;
  std::vector<Player> players;
};

void api_basics_and_validation() {
  World w;
  CHECK(w.call("GET", "/api/status").status == 200);
  CHECK(w.call("GET", "/api/rooms").body.find("rooms")->as_array()->empty());
  auto a = w.post("/api/rooms", "{\"name\":\"Arena 1\",\"game\":\"toy\",\"seats\":3,\"options\":{\"map\":\"a\"}}");
  CHECK(a.status == 201 && *a.body.find("state")->as_string() == "open" && *a.body.find("seat_count")->as_int() == 3);
  CHECK(a.body.find("seats")->as_array()->size() == 3 && a.body.find("seats")->as_array()->at(0).find("name")->is_null());
  const std::string id = *a.body.find("id")->as_string();
  CHECK(id.size() == 8);
  CHECK(w.call("GET", "/api/rooms").body.find("rooms")->as_array()->size() == 1);
  CHECK(*w.call("GET", "/api/rooms/" + id).body.find("name")->as_string() == "Arena 1");
  CHECK(w.call("GET", "/api/rooms/nope").status == 404);
  CHECK(w.call("GET", "/api/rooms/bad%20id").status == 400 || w.call("GET", "/api/rooms/bad%20id").status == 404);
  CHECK(w.call("GET", "/api/rooms/" + id + "/join").status == 405);
  CHECK(w.call("DELETE", "/api/rooms/" + id).status == 405);
  CHECK(w.call("GET", "/elsewhere").status == 404);
  CHECK(w.call("GET", "/api").status == 404);
  for (const char* bad : {"[]", "{", "{}", "{\"name\":\"\"}", "{\"name\":5}", "{\"name\":\" lead\"}", "{\"name\":\"tab\\there\"}",
                          "{\"name\":\"ok\",\"seats\":0}", "{\"name\":\"ok\",\"seats\":17}", "{\"name\":\"ok\",\"seats\":\"2\"}",
                          "{\"name\":\"ok\",\"seats\":1.5}", "{\"name\":\"ok\",\"game\":\"Up Per\"}", "{\"name\":\"ok\",\"game\":\"\"}",
                          "{\"name\":\"ok\",\"options\":[]}"}) {
    CHECK(w.post("/api/rooms", bad).status == 400);
  }
  CHECK(w.post("/api/rooms", "{\"name\":\"" + std::string(41, 'x') + "\"}").status == 400);
  CHECK(w.post("/api/rooms", "{\"name\":\"ok\",\"options\":{\"big\":\"" + std::string(5000, 'x') + "\"}}").status == 400);
  CHECK(w.post("/api/rooms", "{\"name\":\"unicode \xc3\xa9\xe2\x9c\x93\"}").status == 201);
  CHECK(w.lobby->room_count() == 2);
}

void seats_ready_and_secrets() {
  World w;
  const auto room = w.make_room("r", 2);
  const auto other = w.make_room("other", 2);
  const auto a = w.join(room, "ann");
  const auto b = w.join(room, "bob");
  CHECK(a.index == 0 && b.index == 1 && a.secret != b.secret && a.secret.size() == 32);
  CHECK(w.post("/api/rooms/" + room + "/join", "{\"name\":\"cy\"}").status == 409);  // full
  CHECK(w.post("/api/rooms/" + other + "/join", "{\"name\":\"\"}").status == 400);
  CHECK(w.post("/api/rooms/" + other + "/join", "{\"name\":\"" + std::string(25, 'n') + "\"}").status == 400);
  CHECK(w.post("/api/rooms/" + other + "/join", "{\"name\":\"a\\u0001b\"}").status == 400);
  // The seat secret is the only way to act for a seat, and only in its own room.
  CHECK(w.post("/api/rooms/" + room + "/ready", "{\"ready\":true}").status == 401);
  CHECK(w.post("/api/rooms/" + room + "/ready", "{\"ready\":true}", "0123456789abcdef0123456789abcdef").status == 401);
  CHECK(w.post("/api/rooms/" + other + "/ready", "{\"ready\":true}", a.secret).status == 401);
  CHECK(w.post("/api/rooms/" + room + "/ready", "{\"ready\":\"yes\"}", a.secret).status == 400);
  CHECK(w.post("/api/rooms/" + room + "/ready", "{\"ready\":true}", a.secret).status == 200);
  auto view = w.call("GET", "/api/rooms/" + room);
  CHECK(*view.body.find("seats")->as_array()->at(0).find("ready")->as_bool());
  CHECK(*view.body.find("seats")->as_array()->at(0).find("name")->as_string() == "ann");
  // Nothing secret is ever in a view.
  CHECK(view.raw.find(a.secret) == std::string::npos && view.raw.find(b.secret) == std::string::npos);
  CHECK(w.call("GET", "/api/rooms").raw.find(a.secret) == std::string::npos);
  // Leaving frees the seat for someone else, with a new secret; the old one no longer works.
  CHECK(w.post("/api/rooms/" + room + "/leave", "", a.secret).status == 204);
  CHECK(w.post("/api/rooms/" + room + "/ready", "{\"ready\":true}", a.secret).status == 401);
  const auto c = w.join(room, "cy");
  CHECK(c.index == 0 && c.secret != a.secret);
  CHECK(!*w.call("GET", "/api/rooms/" + room).body.find("seats")->as_array()->at(0).find("ready")->as_bool());
  CHECK(w.post("/api/rooms/" + room + "/me", "", c.secret).status == 200);
  CHECK(*w.post("/api/rooms/" + room + "/me", "", c.secret).body.find("state")->as_string() == "open");
}

void long_polling() {
  lobby::LobbyConfig cfg;
  cfg.max_waiters = 2;
  World w(cfg);
  const auto room = w.make_room("lp", 2);
  const auto version = *w.call("GET", "/api/rooms/" + room).body.find("version")->as_int();
  const std::string path = "/api/rooms/" + room + "?since=" + std::to_string(version) + "&wait=20";
  // Three polls: two are held, the third finds the limit and is answered at once.
  std::optional<http::Result> p1, p2;
  http::Client c1, c2;
  c1.request(w.lobby->address(), "l", get_request(path), w.net.now(), [&](const http::Result& r) { p1 = r; });
  c2.request(w.lobby->address(), "l", get_request(path), w.net.now(), [&](const http::Result& r) { p2 = r; });
  for (int i = 0; i < 300; ++i) { w.step(); c1.update(w.net.now()); c2.update(w.net.now()); }
  CHECK(!p1 && !p2);
  auto third = w.call("GET", path);
  CHECK(third.status == 200 && *third.body.find("version")->as_int() == version);  // unchanged, at once
  // A change wakes both, with the new version.
  w.join(room, "ann");
  for (int i = 0; i < 300 && !(p1 && p2); ++i) { w.step(); c1.update(w.net.now()); c2.update(w.net.now()); }
  CHECK(p1 && p2 && p1->ok && p2->ok);
  auto v = json::parse(p1->body);
  CHECK(*v->find("version")->as_int() > version && *v->find("taken")->as_int() == 1);
  // A poll nothing wakes returns after its wait, unchanged.
  const auto now_version = *v->find("version")->as_int();
  std::optional<http::Result> p3;
  c1.request(w.lobby->address(), "l", get_request("/api/rooms/" + room + "?since=" + std::to_string(now_version) + "&wait=1"),
             w.net.now(), [&](const http::Result& r) { p3 = r; });
  for (int i = 0; i < 400; ++i) { w.step(); c1.update(w.net.now()); }
  CHECK(!p3);
  for (int i = 0; i < 1200 && !p3; ++i) { w.step(); c1.update(w.net.now()); }
  CHECK(p3 && p3->ok && *json::parse(p3->body)->find("version")->as_int() == now_version);
  // A different version is answered at once, and so is a malformed query.
  CHECK(w.call("GET", "/api/rooms/" + room + "?since=1&wait=20").status == 200);
  CHECK(w.call("GET", "/api/rooms/" + room + "?since=x&wait=20").status == 200);
}

void the_whole_path_to_a_played_match() {
  World w;
  w.start_game_server();
  // The game server registers itself with the lobby, as it would from its own config.
  const std::string control = "127.0.0.1:" + std::to_string(w.gs->control_address().port);
  CHECK(w.control("/control/servers", "{\"control\":\"" + control + "\",\"udp\":\"game.example:7000\",\"max_matches\":4,\"matches\":0}").status == 200);
  const auto room = w.make_room("final", 2, "{\"end_after\":180}");
  const auto a = w.join(room, "ann");
  const auto b = w.join(room, "bob");
  w.ready(room, a);
  CHECK(w.state(room) == "open");
  w.ready(room, b);
  w.run(300'000);
  CHECK(w.state(room) == "playing");
  CHECK(w.gs->match_count() == 1);
  auto ma = w.post("/api/rooms/" + room + "/me", "", a.secret);
  auto mb = w.post("/api/rooms/" + room + "/me", "", b.secret);
  CHECK(ma.status == 200 && *ma.body.find("state")->as_string() == "playing");
  CHECK(*ma.body.find("room_id")->as_string() == room && *ma.body.find("server")->find("udp")->as_string() == "game.example:7000");
  const std::string ta = *ma.body.find("token")->as_string(), tb = *mb.body.find("token")->as_string();
  CHECK(ta.size() == 32 && tb.size() == 32 && ta != tb && *ma.body.find("seat")->as_int() == 0);
  // Each token is only in its own answer, and never in a public view.
  CHECK(ma.raw.find(tb) == std::string::npos && w.call("GET", "/api/rooms/" + room).raw.find(ta) == std::string::npos);
  CHECK(w.post("/api/rooms/" + room + "/me", "").status == 401);
  CHECK(w.post("/api/rooms/" + room + "/leave", "", a.secret).status == 409);  // started
  CHECK(w.post("/api/rooms/" + room + "/join", "{\"name\":\"late\"}").status == 409);
  w.connect_player(room, ta, 1);
  w.connect_player(room, tb, 2);
  w.run(2'000'000);
  for (auto& p : w.players) CHECK(p.session->state() == session::ClientSession::State::Running);
  CHECK(w.gs->stats().joins_ok == 2);
  // The game ends; the game server reports it to the lobby... which it is not configured to do here,
  // so the test plays the game server's part with the result it would send.
  w.run(6'000'000);
  CHECK(w.gs->match_count() == 0);
  auto res = w.control("/control/rooms/" + room + "/result", "{\"outcome\":\"completed\",\"frames\":180,\"joined\":[true,true],\"result\":{\"steps\":180}}");
  CHECK(res.status == 200);
  auto fin = w.call("GET", "/api/rooms/" + room);
  CHECK(*fin.body.find("state")->as_string() == "finished" && *fin.body.find("result")->find("outcome")->as_string() == "completed");
  CHECK(*fin.body.find("result")->find("game")->find("steps")->as_int() == 180);
  CHECK(*w.post("/api/rooms/" + room + "/me", "", a.secret).body.find("state")->as_string() == "finished");
  CHECK(w.post("/api/rooms/" + room + "/me", "", a.secret).raw.find(ta) == std::string::npos);
  CHECK(w.post("/api/rooms/" + room + "/leave", "", a.secret).status == 204);
  CHECK(w.post("/api/rooms/" + room + "/result", "{}", kSecret).status == 404);  // reporting results is not a player action
}

void no_server_then_a_server() {
  World w;
  const auto room = w.make_room("ns", 2);
  const auto a = w.join(room, "ann"), b = w.join(room, "bob");
  w.ready(room, a);
  w.ready(room, b);
  w.run(100'000);
  auto v = w.call("GET", "/api/rooms/" + room);
  CHECK(*v.body.find("state")->as_string() == "open" && *v.body.find("error")->as_string() == "no_server");
  CHECK(!*v.body.find("seats")->as_array()->at(0).find("ready")->as_bool());
  CHECK(w.lobby->stats().assignments_failed == 1);
  FakeServer ok(201);
  w.fakes.push_back(&ok);
  w.register_fake(ok);
  w.ready(room, a);
  w.ready(room, b);
  w.run(200'000);
  CHECK(w.state(room) == "playing" && ok.requests == 1);
  // What the game server was sent: the room, a token per seat, the options.
  auto body = json::parse(ok.last_body);
  CHECK(*body->find("room_id")->as_string() == room && body->find("seats")->as_array()->size() == 2);
  CHECK(body->find("seats")->as_array()->at(0).find("token")->as_string()->size() == 32);
}

void failing_over_between_servers() {
  World w;
  FakeServer full(409), broken(500), good(201);
  w.fakes = {&full, &broken, &good};
  w.register_fake(full, 8);
  w.register_fake(broken, 8);
  w.register_fake(good, 2);
  const auto room = w.make_room("fo", 2);
  const auto a = w.join(room, "ann"), b = w.join(room, "bob");
  w.ready(room, a);
  w.ready(room, b);
  w.run(500'000);
  CHECK(w.state(room) == "playing");
  CHECK(full.requests == 1 && broken.requests == 1 && good.requests == 1);  // each tried once, then the one that took it
  // The refusing servers are left alone for a while: the next room goes straight to the good one.
  const auto room2 = w.make_room("fo2", 1);
  const auto c = w.join(room2, "cy");
  w.ready(room2, c);
  w.run(300'000);
  CHECK(w.state(room2) == "playing" && full.requests == 1 && broken.requests == 1 && good.requests == 2);
  // The good server said it holds 2 matches of 2: a third room has nowhere to go.
  const auto room3 = w.make_room("fo3", 1);
  const auto d = w.join(room3, "di");
  w.ready(room3, d);
  w.run(300'000);
  auto v = w.call("GET", "/api/rooms/" + room3);
  CHECK(*v.body.find("state")->as_string() == "open" && *v.body.find("error")->as_string() == "no_server");
}

void a_lost_server_fails_its_rooms() {
  lobby::LobbyConfig cfg;
  cfg.server_ttl = 2'000'000;
  World w(cfg);
  FakeServer s(201);
  w.fakes.push_back(&s);
  w.register_fake(s);
  const auto room = w.make_room("lost", 1);
  w.ready(room, w.join(room, "ann"));
  w.run(300'000);
  CHECK(w.state(room) == "playing");
  w.run(3'000'000);  // no more heartbeats
  auto v = w.call("GET", "/api/rooms/" + room);
  CHECK(*v.body.find("state")->as_string() == "finished" && *v.body.find("result")->find("outcome")->as_string() == "server_lost");
}

void the_control_side_is_authenticated_and_checked() {
  World w;
  FakeServer s(201);
  const std::string ok = "{\"control\":\"" + s.control() + "\",\"udp\":\"u:1\",\"max_matches\":2,\"matches\":0}";
  CHECK(w.call("POST", "/control/servers", ok).status == 401);
  CHECK(w.call("POST", "/control/servers", ok, "wrong").status == 401);
  CHECK(w.call("POST", "/control/servers", ok, std::string(kSecret) + "x").status == 401);
  CHECK(w.lobby->stats().unauthorized == 3 && w.lobby->server_count() == 0);
  for (const char* bad : {"[]", "{}", "{\"control\":\"nonsense\",\"udp\":\"u\",\"max_matches\":2,\"matches\":0}",
                          "{\"control\":\"h:1\",\"udp\":\"\",\"max_matches\":2,\"matches\":0}",
                          "{\"control\":\"h:1\",\"udp\":\"u\",\"max_matches\":0,\"matches\":0}",
                          "{\"control\":\"h:1\",\"udp\":\"u\",\"max_matches\":2,\"matches\":-1}",
                          "{\"control\":\"h:1\",\"udp\":\"u\",\"max_matches\":\"2\",\"matches\":0}"}) {
    CHECK(w.control("/control/servers", bad).status == 400);
  }
  CHECK(w.control("/control/servers", ok).status == 200 && w.lobby->server_count() == 1);
  CHECK(w.control("/control/servers", ok).status == 200 && w.lobby->server_count() == 1);  // the heartbeat
  CHECK(w.call("GET", "/control/servers", "", kSecret).status == 405);
  CHECK(w.control("/control/rooms/missing/result", "{}").status == 404);
  const auto room = w.make_room("c", 1);
  CHECK(w.control("/control/rooms/" + room + "/result", "{\"outcome\":\"completed\"}").status == 409);  // not playing
  CHECK(w.call("POST", "/control/rooms/" + room + "/result", "{}", "wrong").status == 401);
}

void writes_are_rate_limited_per_address() {
  lobby::LobbyConfig cfg;
  cfg.write_burst = 5;
  cfg.writes_per_second = 0.5;
  World w(cfg);
  int created = 0, limited = 0;
  for (int i = 0; i < 12; ++i) {
    const int st = w.post("/api/rooms", "{\"name\":\"spam\"}").status;
    created += st == 201;
    limited += st == 429;
  }
  CHECK(created >= 5 && created <= 8 && limited >= 4);
  CHECK(w.call("GET", "/api/rooms").status == 200);  // reads are not limited
  CHECK(w.lobby->stats().rate_limited == static_cast<std::uint64_t>(limited));
  w.run(10'000'000);  // the bucket refills
  CHECK(w.post("/api/rooms", "{\"name\":\"again\"}").status == 201);
}

void absent_players_and_stale_rooms_are_cleaned_up() {
  lobby::LobbyConfig cfg;
  cfg.seat_presence = 3'000'000;
  cfg.empty_room_ttl = 6'000'000;
  World w(cfg);
  const auto room = w.make_room("gc", 2);
  const auto a = w.join(room, "ann");
  w.join(room, "bob");
  w.run(2'000'000);
  CHECK(w.post("/api/rooms/" + room + "/me", "", a.secret).status == 200);  // ann shows she is there
  w.run(2'000'000);
  auto v = w.call("GET", "/api/rooms/" + room);
  CHECK(*v.body.find("taken")->as_int() == 1);  // bob was away too long, ann was not
  CHECK(v.body.find("seats")->as_array()->at(0).find("name")->as_string() != nullptr);
  w.run(4'000'000);  // now ann too
  CHECK(*w.call("GET", "/api/rooms/" + room).body.find("taken")->as_int() == 0);
  w.run(7'000'000);  // and the empty room goes
  CHECK(w.call("GET", "/api/rooms/" + room).status == 404 && w.lobby->room_count() == 0);
  lobby::LobbyConfig small;
  small.max_rooms = 2;
  World full(small);
  full.make_room("1");
  full.make_room("2");
  CHECK(full.post("/api/rooms", "{\"name\":\"3\"}").status == 503);
}

void random_requests_do_no_harm() {
  lobby::LobbyConfig cfg;
  cfg.write_burst = 1e9;
  cfg.max_rooms = 20;
  World w(cfg);
  FakeServer fake(201);
  w.fakes.push_back(&fake);
  w.register_fake(fake, 100);
  const auto room = w.make_room("fuzz", 2);
  const auto seat = w.join(room, "ann");
  std::mt19937_64 rng(5);
  const char* methods[] = {"GET", "POST", "PUT", "DELETE"};
  const std::vector<std::string> segments = {"api", "control", "rooms", "servers", "status", "join", "ready", "leave", "me",
                                             "result", room, "zzzz", "..", "", "%41", "a b"};
  const std::vector<std::string> bodies = {"", "{}", "[]", "null", "{\"name\":\"x\"}", "{\"ready\":true}", "{\"ready\":1}",
                                           "{\"seats\":99}", "{\"name\":\"n\",\"seats\":3,\"game\":\"g\"}", "{\"outcome\":\"completed\"}",
                                           "{\"control\":\"h:1\",\"udp\":\"u\",\"max_matches\":1,\"matches\":0}", "{\"a\":", "\xff\xfe"};
  const std::vector<std::string> bearers = {"", seat.secret, kSecret, "x", std::string(300, 'y')};
  for (int i = 0; i < 1500; ++i) {
    std::string path;
    const std::size_t n = 1 + rng() % 4;
    for (std::size_t k = 0; k < n; ++k) path += "/" + segments[rng() % segments.size()];
    if (rng() % 4 == 0) path += "?since=" + std::to_string(rng() % 5) + "&wait=" + std::to_string(rng() % 3);
    const std::string& bearer = bearers[rng() % bearers.size()];
    http::OutgoingRequest q;
    q.method = methods[rng() % 4];
    q.path = path;
    q.body = bodies[rng() % bodies.size()];
    if (!bearer.empty()) q.headers.emplace_back("Authorization", "Bearer " + bearer);
    std::optional<http::Result> got;
    if (!w.caller.request(w.lobby->address(), "l", q, w.net.now(), [&](const http::Result& r) { got = r; })) continue;
    for (int k = 0; k < 3000 && !got; ++k) w.step();
    CHECK(got.has_value());
    if (got->ok) {
      const int st = got->status;
      CHECK(st == 200 || st == 201 || st == 204 || st == 400 || st == 401 || st == 404 || st == 405 || st == 409 || st == 503 || st == 414);
    }
  }
  CHECK(w.call("GET", "/api/status").status == 200);
}

void the_home_page() {
  lobby::LobbyConfig cfg;
  auto site = lobby::parse_site_config("{\"title\":\"Toy Brawl\",\"download\":{\"url\":\"https://x.example/get\"}}");
  CHECK(site);
  cfg.site = *site;
  World w(cfg);
  auto home = w.call("GET", "/");
  CHECK(home.status == 200 && home.header("content-type")->rfind("text/html", 0) == 0);
  CHECK(home.raw.find("<title>") != std::string::npos && home.raw.find("/app.js") != std::string::npos);
  CHECK(home.header("content-security-policy")->find("default-src 'none'") == 0);
  CHECK(home.header("content-security-policy")->find("script-src 'self'") != std::string::npos);
  CHECK(home.header("content-security-policy")->find("unsafe-inline") == std::string::npos);
  CHECK(*home.header("x-frame-options") == "DENY" && *home.header("x-content-type-options") == "nosniff" &&
        *home.header("referrer-policy") == "no-referrer");
  CHECK(w.call("GET", "/index.html").status == 200 && w.call("GET", "/index.html").raw == home.raw);
  auto css = w.call("GET", "/style.css");
  auto js = w.call("GET", "/app.js");
  CHECK(css.status == 200 && css.header("content-type")->rfind("text/css", 0) == 0);
  CHECK(js.status == 200 && js.header("content-type")->rfind("text/javascript", 0) == 0);
  CHECK(w.call("GET", "/nothing").status == 404 && w.call("GET", "/api").status == 404);
  CHECK(w.call("POST", "/").status == 405 && w.call("DELETE", "/app.js").status == 405);
  CHECK(w.call("GET", "/../secret").status == 400 && w.call("GET", "/%2e%2e/secret").status == 400);
  auto text = w.call("GET", "/api/site");
  CHECK(text.status == 200 && *text.body.find("title")->as_string() == "Toy Brawl" &&
        *text.body.find("download")->find("url")->as_string() == "https://x.example/get" && text.body.find("steps")->as_array()->size() == 3);
  // The page is public: it needs no secret, and shows no secret.
  CHECK(home.raw.find(kSecret) == std::string::npos && text.raw.find(kSecret) == std::string::npos);
  // The text of rooms goes to the page as data: the API returns it as JSON, escaped.
  w.make_room("<script>alert(1)</script>");
  auto rooms = w.call("GET", "/api/rooms");
  CHECK(*rooms.header("content-type") == "application/json" && *rooms.header("x-content-type-options") == "nosniff");
}

void a_site_directory_replaces_the_page() {
  const auto dir = std::filesystem::temp_directory_path() / ("npk-lobby-site-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "index.html") << "<html><body>my own page</body></html>";
  std::ofstream(dir / "extra.css") << "x{}";
  lobby::LobbyConfig cfg;
  cfg.site_dir = dir;
  World w(cfg);
  CHECK(w.call("GET", "/").raw.find("my own page") != std::string::npos);
  CHECK(w.call("GET", "/extra.css").status == 200);
  CHECK(w.call("GET", "/app.js").status == 200);  // not in the directory: the default one
  CHECK(w.call("GET", "/.hidden").status == 404 && w.call("GET", "/extra.exe").status == 404);
  std::filesystem::remove_all(dir);
}

}  // namespace

void options_travel_with_the_room() {
  World w;
  FakeServer ok(201);
  w.fakes.push_back(&ok);
  w.register_fake(ok);
  const std::string opts = "{\"stocks\":3,\"stage\":\"Quarry\",\"picks\":[\"a\",\"b\"],\"nested\":{\"x\":null}}";
  const std::string room = w.make_room("opt", 2, opts);
  const std::string want = json::dump(*json::parse(opts));
  auto dumped = [](const json::Value* v) { return v ? json::dump(*v) : std::string("<missing>"); };
  // The room read, the room list and the join response all carry the same value.
  CHECK(dumped(w.call("GET", "/api/rooms/" + room).body.find("options")) == want);
  const json::Value list = w.call("GET", "/api/rooms").body;
  CHECK(dumped(list.find("rooms")->as_array()->at(0).find("options")) == want);
  auto j = w.post("/api/rooms/" + room + "/join", "{\"name\":\"ann\"}");
  CHECK(j.status == 200 && dumped(j.body.find("room")->find("options")) == want);
  const World::Seat a{*j.body.find("seat")->as_int(), *j.body.find("secret")->as_string()};
  auto me = w.post("/api/rooms/" + room + "/me", "", a.secret);
  CHECK(me.status == 200 && dumped(me.body.find("options")) == want);
  // Never the secrets.
  CHECK(w.call("GET", "/api/rooms").raw.find(a.secret) == std::string::npos);
  // Immutable: no endpoint takes options after creation.
  CHECK(w.post("/api/rooms/" + room + "/ready", "{\"ready\":true,\"options\":{\"stocks\":9}}", a.secret).status == 200);
  CHECK(dumped(w.call("GET", "/api/rooms/" + room).body.find("options")) == want);
  // The game server is handed the same value when the match starts.
  const auto b = w.join(room, "bob");
  w.ready(room, b);
  w.run(200'000);
  CHECK(w.state(room) == "playing");
  CHECK(dumped(json::parse(ok.last_body)->find("options")) == want);
  CHECK(dumped(w.post("/api/rooms/" + room + "/me", "", a.secret).body.find("options")) == want);
  // A string works too; no options means {}; the cap is 1 KiB serialised.
  auto s = w.post("/api/rooms", "{\"name\":\"s\",\"options\":\"3 stocks, Quarry\"}");
  CHECK(s.status == 201 && *s.body.find("options")->as_string() == "3 stocks, Quarry");
  auto none = w.post("/api/rooms", "{\"name\":\"n\"}");
  CHECK(none.status == 201 && none.body.find("options")->is_object() && none.body.find("options")->as_object()->empty());
  for (const char* bad : {"5", "true", "null", "[1]"}) {
    CHECK(w.post("/api/rooms", std::string("{\"name\":\"b\",\"options\":") + bad + "}").status == 400);
  }
  CHECK(w.post("/api/rooms", "{\"name\":\"b\",\"options\":\"" + std::string(1022, 'x') + "\"}").status == 201);  // 1024 with quotes
  CHECK(w.post("/api/rooms", "{\"name\":\"b\",\"options\":\"" + std::string(1023, 'x') + "\"}").status == 400);
}

int main() {
  options_travel_with_the_room();
  api_basics_and_validation();
  seats_ready_and_secrets();
  long_polling();
  the_whole_path_to_a_played_match();
  no_server_then_a_server();
  failing_over_between_servers();
  a_lost_server_fails_its_rooms();
  the_control_side_is_authenticated_and_checked();
  writes_are_rate_limited_per_address();
  absent_players_and_stale_rooms_are_cleaned_up();
  the_home_page();
  a_site_directory_replaces_the_page();
  random_requests_do_no_harm();
  std::puts("lobby_test: ok");
  return 0;
}
