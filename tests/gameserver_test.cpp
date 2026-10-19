// The game server over a simulated UDP network (for the matches) and real
// loopback TCP (for the control API and the fake lobby), all stepped together.

#include <cstdio>
#include <functional>
#include <map>

#include "check.hpp"
#include "npk/gameserver/game_server.hpp"
#include "npk/session/client_session.hpp"
#include "npk/transport/client.hpp"
#include "npk/transport/sim_link.hpp"
#include "toy_game.hpp"

using namespace npk;
using npk::session::Frame;
using npk::transport::Address;
using npk::transport::Micros;

namespace {

constexpr const char* kSecret = "correct-horse-battery-staple";
const Address kServer = Address::v4(10, 0, 0, 1, 7000);

std::string hex_token(int n) {
  std::string t;
  for (int i = 0; i < 16; ++i) {
    char b[3];
    std::snprintf(b, sizeof b, "%02x", (n * 31 + i * 7) & 0xff);
    t += b;
  }
  return t;
}

std::array<std::uint8_t, 16> raw_token(int n) {
  std::array<std::uint8_t, 16> t{};
  for (int i = 0; i < 16; ++i) t[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((n * 31 + i * 7) & 0xff);
  return t;
}

std::string room_body(const std::string& id, std::vector<int> tokens, std::string options = "{}") {
  std::string seats;
  for (int t : tokens) seats += std::string(seats.empty() ? "" : ",") + "{\"token\":\"" + hex_token(t) + "\"}";
  return "{\"room_id\":\"" + id + "\",\"seats\":[" + seats + "],\"options\":" + options + "}";
}

// A lobby that records what the game server sends it.
struct FakeLobby {
  FakeLobby() {
    server = http::Server::listen(Address::loopback_v4(0), [this](const http::Request& r, const Address&, http::Responder rs) {
      if (!http::constant_time_equal(http::bearer_token(r), kSecret)) {
        ++unauthorized;
        rs.send(http::Response::empty(401));
        return;
      }
      if (r.path == "/control/servers") registrations.push_back(r.body);
      else results.emplace_back(r.path, r.body);
      if (fail_results > 0 && r.path != "/control/servers") {
        --fail_results;
        rs.send(http::Response::empty(503));
        return;
      }
      rs.send(http::Response::json(200, "{}"));
    });
    CHECK(server);
  }
  std::unique_ptr<http::Server> server;
  std::vector<std::string> registrations;
  std::vector<std::pair<std::string, std::string>> results;
  int unauthorized = 0, fail_results = 0;
};

struct Fixture {
  struct Player {
    std::unique_ptr<transport::SimLink> link;
    std::unique_ptr<ToyGame> game;
    std::unique_ptr<session::ClientSession> session;
    bool silent = false;
  };

  explicit Fixture(gameserver::GameServerConfig cfg = {}, bool with_lobby = false) : net(5) {
    net.set_default_conditions({.delay = 10'000, .jitter = 3'000});
    link = net.make_link(kServer);
    if (!cfg.factory) {
      cfg.factory = [](std::size_t n, const json::Value& o) -> std::unique_ptr<session::Simulation> {
        if (n > ToyGame::kMaxPlayers) return nullptr;
        if (const auto* bad = o.find("refuse"); bad && bad->as_bool().value_or(false)) return nullptr;
        auto g = std::make_unique<ToyGame>(n);
        if (const auto* e = o.find("end_after"); e && e->as_int()) g->end_after = static_cast<Frame>(*e->as_int());
        return g;
      };
    }
    cfg.control_secret = cfg.control_secret.empty() ? kSecret : cfg.control_secret;
    cfg.control_bind = Address::loopback_v4(0);
    cfg.transport.seed = 99;
    if (with_lobby) {
      lobby = std::make_unique<FakeLobby>();
      cfg.lobby = "127.0.0.1:" + std::to_string(lobby->server->local_address().port);
      cfg.advertise_control = "gameserver:9000";
      cfg.advertise_udp = "play.example:7000";
      cfg.register_interval = 2'000'000;
      cfg.result_retry = 500'000;
    }
    std::string err;
    gs = gameserver::GameServer::create(*link, std::move(cfg), &err);
    if (!gs) std::fprintf(stderr, "create: %s\n", err.c_str());
    CHECK(gs);
  }

  void step() {
    gs->update(net.now());
    if (lobby) lobby->server->update(net.now());
    caller.update(net.now());
    for (auto& p : players) {
      if (!p.silent) p.session->update(net.now());
    }
    for (auto* c : pumped) c->update(net.now());
    net.advance(1000);
  }
  void run(Micros d) {
    for (Micros t = 0; t < d; t += 1000) step();
  }

  // A call to the control API as the lobby would make it.
  http::Result call(const std::string& method, const std::string& path, const std::string& body,
                    const std::string& secret = kSecret) {
    http::OutgoingRequest q;
    q.method = method;
    q.path = path;
    q.body = body;
    if (!secret.empty()) q.headers.emplace_back("Authorization", "Bearer " + secret);
    std::optional<http::Result> got;
    CHECK(caller.request(gs->control_address(), "gameserver", q, net.now(), [&](const http::Result& r) { got = r; }));
    for (int i = 0; i < 3000 && !got; ++i) step();
    CHECK(got.has_value());
    return *got;
  }
  int status(const http::Result& r) { return r.ok ? r.status : -1; }

  Player& add_player(const std::string& room, int token, std::uint8_t host, Frame end = 0) {
    Player p;
    p.link = net.make_link(Address::v4(10, 1, 0, host, 5000));
    p.game = std::make_unique<ToyGame>(2);
    (void)end;
    session::ClientSessionConfig cc;
    cc.transport.seed = host * 13u;
    cc.join = session::msg::Join{room, raw_token(token)};
    auto script = std::make_shared<std::vector<std::uint8_t>>(toy_inputs(host, 1, 20000));
    p.session = std::make_unique<session::ClientSession>(
        *p.link, kServer, *p.game,
        [script](Frame w, std::span<std::uint8_t> o) {
          o[0] = (*script)[(w % 20000) * 2];
          o[1] = (*script)[(w % 20000) * 2 + 1];
        },
        cc);
    p.session->connect(net.now());
    players.push_back(std::move(p));
    return players.back();
  }

  transport::SimNetwork net;
  std::unique_ptr<transport::SimLink> link;
  std::unique_ptr<FakeLobby> lobby;
  std::unique_ptr<gameserver::GameServer> gs;
  http::Client caller;
  std::vector<Player> players;
  std::vector<transport::Client*> pumped;  // bare clients the test runs along
};

using Players = std::vector<Fixture::Player>;

void creating_a_game_and_playing_it() {
  Fixture f;
  auto r = f.call("POST", "/rooms", room_body("alpha", {1, 2}));
  CHECK(f.status(r) == 201);
  CHECK(f.gs->match_count() == 1);
  f.add_player("alpha", 1, 1);
  f.add_player("alpha", 2, 2);
  f.run(6'000'000);
  for (auto& p : f.players) CHECK(p.session->state() == session::ClientSession::State::Running);
  CHECK(f.gs->stats().joins_ok == 2);
  // Both are in the same match: seats 0 and 1, and the same canonical history.
  CHECK(f.players[0].session->player() != f.players[1].session->player());
  CHECK(!f.players[0].session->checkpoints().empty());
  const auto a = f.players[0].session->checkpoints(), b = f.players[1].session->checkpoints();
  for (const auto& x : a) {
    for (const auto& y : b) {
      if (x.frame == y.frame) CHECK(x.hash == y.hash);
    }
  }
  auto s = json::parse(f.call("GET", "/status", "").body);
  CHECK(s && *s->find("matches")->as_int() == 1 && s->find("rooms")->as_array()->size() == 1);
}

void many_matches_at_once_and_capacity() {
  gameserver::GameServerConfig cfg;
  cfg.max_matches = 2;
  Fixture f(std::move(cfg));
  CHECK(f.status(f.call("POST", "/rooms", room_body("a", {1, 2}))) == 201);
  CHECK(f.status(f.call("POST", "/rooms", room_body("b", {3, 4}))) == 201);
  auto full = f.call("POST", "/rooms", room_body("c", {5, 6}));
  CHECK(f.status(full) == 409 && json::parse(full.body)->find("error")->as_string()->compare("full") == 0);
  auto dup = f.call("POST", "/rooms", room_body("a", {7, 8}));
  CHECK(f.status(dup) == 409);  // full is checked after exists
  // Two matches play at the same time, each with its own pair.
  f.add_player("a", 1, 1);
  f.add_player("a", 2, 2);
  f.add_player("b", 3, 3);
  f.add_player("b", 4, 4);
  f.run(5'000'000);
  for (auto& p : f.players) CHECK(p.session->state() == session::ClientSession::State::Running);
  CHECK(f.players[0].session->checkpoints().size() >= 3 && f.players[2].session->checkpoints().size() >= 3);
  // Cancelling frees a place and ends that match's connections.
  CHECK(f.status(f.call("DELETE", "/rooms/a", "")) == 204);
  CHECK(f.status(f.call("DELETE", "/rooms/a", "")) == 404);
  f.run(1'000'000);
  CHECK(f.gs->match_count() == 1);
  CHECK(f.players[0].session->state() == session::ClientSession::State::Ended);
  CHECK(f.players[2].session->state() == session::ClientSession::State::Running);
  CHECK(f.status(f.call("POST", "/rooms", room_body("c", {5, 6}))) == 201);
}

void the_control_api_needs_the_secret_and_checks_its_input() {
  Fixture f;
  CHECK(f.status(f.call("POST", "/rooms", room_body("x", {1}), "")) == 401);
  CHECK(f.status(f.call("POST", "/rooms", room_body("x", {1}), "wrong-secret-of-some-length")) == 401);
  CHECK(f.status(f.call("POST", "/rooms", room_body("x", {1}), std::string(kSecret).substr(0, 20))) == 401);
  CHECK(f.status(f.call("POST", "/rooms", room_body("x", {1}), std::string(kSecret) + "x")) == 401);
  CHECK(f.status(f.call("GET", "/status", "", "")) == 401);
  CHECK(f.gs->stats().unauthorized == 5 && f.gs->match_count() == 0);

  const std::string good = room_body("x", {1, 2});
  for (const std::string& bad : {
           std::string("[]"), std::string("{"), std::string("{}"),
           room_body("bad id", {1}), room_body("", {1}), room_body(std::string(33, 'a'), {1}),
           std::string("{\"room_id\":\"x\",\"seats\":[]}"), std::string("{\"room_id\":\"x\",\"seats\":{}}"),
           room_body("x", {1, 1}),  // the same token twice
           std::string("{\"room_id\":\"x\",\"seats\":[{\"token\":\"abc\"}]}"),
           std::string("{\"room_id\":\"x\",\"seats\":[{\"token\":\"") + std::string(32, 'g') + "\"}]}",
           std::string("{\"room_id\":\"x\",\"seats\":[{}]}"),
           room_body("x", {1, 2}, "[]"),         // options must be an object
           room_body("x", {1, 2}, "{\"refuse\":true}"),  // the game declines
           room_body("x", {1, 2, 3, 4, 5}),      // more seats than the game supports
       }) {
    CHECK(f.status(f.call("POST", "/rooms", bad)) == 400);
  }
  CHECK(f.gs->match_count() == 0);
  CHECK(f.status(f.call("GET", "/rooms", "")) == 405);
  CHECK(f.status(f.call("GET", "/elsewhere", "")) == 404);
  CHECK(f.status(f.call("PUT", "/rooms", good)) == 405);
  CHECK(f.status(f.call("POST", "/status", "")) == 405);
  CHECK(f.status(f.call("POST", "/rooms", good)) == 201);
}

void a_short_secret_or_no_factory_is_refused() {
  transport::SimNetwork net(1);
  auto link = net.make_link(kServer);
  gameserver::GameServerConfig cfg;
  cfg.control_bind = Address::loopback_v4(0);
  std::string err;
  CHECK(!gameserver::GameServer::create(*link, cfg, &err) && !err.empty());  // no factory
  cfg.factory = [](std::size_t, const json::Value&) { return std::unique_ptr<session::Simulation>(); };
  CHECK(!gameserver::GameServer::create(*link, cfg, &err));  // no secret
  cfg.control_secret = "short";
  CHECK(!gameserver::GameServer::create(*link, cfg, &err));
  cfg.control_secret = kSecret;
  CHECK(gameserver::GameServer::create(*link, cfg, &err));
}

// A bare transport client that tries to join with the given room and token.
struct Joiner {
  Joiner(Fixture& f, std::uint8_t host) {
    link = f.net.make_link(Address::v4(10, 2, 0, host, 5000));
    client = std::make_unique<transport::Client>(*link, kServer, transport::ClientConfig{.seed = host * 7u});
    client->connect(f.net.now());
    f.pumped.push_back(client.get());
  }
  void join(const std::string& room, int token) {
    std::vector<std::uint8_t> m;
    session::msg::encode_join({room, raw_token(token)}, m);
    client->send_reliable(m);
  }
  std::unique_ptr<transport::SimLink> link;
  std::unique_ptr<transport::Client> client;
};

void only_the_right_token_gets_a_seat() {
  Fixture f;
  CHECK(f.status(f.call("POST", "/rooms", room_body("r1", {1, 2}))) == 201);
  CHECK(f.status(f.call("POST", "/rooms", room_body("r2", {3, 4}))) == 201);
  struct Try { const char* room; int token; bool ok; };
  const Try tries[] = {{"r1", 99, false}, {"nope", 1, false}, {"r2", 1, false}, {"r1", 1, true}, {"r1", 1, false},  // already taken
                       {"r1", 2, true}, {"r2", 4, true}};
  std::uint8_t host = 1;
  std::vector<std::unique_ptr<Joiner>> js;
  for (const Try& t : tries) {
    auto j = std::make_unique<Joiner>(f, host++);
    f.run(300'000);  // connected
    j->join(t.room, t.token);
    f.run(300'000);
    CHECK((j->client->state() == transport::Client::State::Connected) == true);  // a first failure does not close
    js.push_back(std::move(j));
  }
  CHECK(f.gs->stats().joins_ok == 3 && f.gs->stats().joins_failed == 4);
}

void bad_joiners_are_closed() {
  gameserver::GameServerConfig cfg;
  cfg.unjoined_timeout = 2'000'000;
  Fixture f(std::move(cfg));
  CHECK(f.status(f.call("POST", "/rooms", room_body("r", {1, 2}))) == 201);
  // Three wrong tries and the connection is closed.
  Joiner a(f, 1);
  f.run(300'000);
  for (int i = 0; i < 3; ++i) {
    a.join("r", 50 + i);
    f.run(300'000);
  }
  CHECK(a.client->state() == transport::Client::State::Closed);
  // Garbage instead of a Join counts as a failure too, on either channel.
  Joiner b(f, 2);
  f.run(300'000);
  b.client->send_unreliable(std::vector<std::uint8_t>{1, 2, 3});
  b.client->send_reliable(std::vector<std::uint8_t>{});
  b.client->send_reliable(std::vector<std::uint8_t>(40, 0xff));
  f.run(500'000);
  CHECK(b.client->state() == transport::Client::State::Closed);
  // One that says nothing is dropped at the unjoined timeout.
  Joiner c(f, 3);
  f.run(1'000'000);
  CHECK(c.client->state() == transport::Client::State::Connected);
  f.run(2'000'000);
  CHECK(c.client->state() == transport::Client::State::Closed);
  CHECK(f.gs->stats().unjoined_timeouts >= 1);
  // A player who joined cannot be locked out by those: the right token still works.
  Joiner d(f, 4);
  f.run(300'000);
  d.join("r", 1);
  f.run(300'000);
  CHECK(f.gs->stats().joins_ok == 1);
}

void a_finished_match_reports_its_result() {
  Fixture f({}, true);
  CHECK(f.status(f.call("POST", "/rooms", room_body("g1", {1, 2}, "{\"end_after\":240}"))) == 201);
  f.add_player("g1", 1, 1);
  f.add_player("g1", 2, 2);
  f.run(8'000'000);
  CHECK(f.gs->match_count() == 0);
  CHECK(f.gs->stats().results_sent == 1);
  CHECK(f.lobby->results.size() == 1 && f.lobby->results[0].first == "/control/rooms/g1/result");
  auto v = json::parse(f.lobby->results[0].second);
  CHECK(v && *v->find("room_id")->as_string() == "g1" && *v->find("outcome")->as_string() == "completed");
  CHECK(*v->find("frames")->as_int() == 240);
  CHECK(v->find("joined")->as_array()->size() == 2 && *v->find("joined")->as_array()->at(0).as_bool());
  CHECK(v->find("result")->find("hp")->as_array()->size() == 2 && *v->find("result")->find("steps")->as_int() == 240);
  // The clients saw the end of the connection.
  for (auto& p : f.players) CHECK(p.session->state() == session::ClientSession::State::Ended);
  // The slot is free again.
  CHECK(f.status(f.call("POST", "/rooms", room_body("g2", {3, 4}))) == 201);
}

void registration_is_repeated_and_authenticated() {
  Fixture f({}, true);
  f.run(7'000'000);
  CHECK(f.lobby->registrations.size() >= 3);
  auto v = json::parse(f.lobby->registrations[0]);
  CHECK(v && *v->find("control")->as_string() == "gameserver:9000" && *v->find("udp")->as_string() == "play.example:7000");
  CHECK(*v->find("max_matches")->as_int() == 16 && *v->find("matches")->as_int() == 0);
  CHECK(f.lobby->unauthorized == 0 && f.gs->stats().registrations_ok >= 3);
}

void results_are_retried_and_then_dropped() {
  gameserver::GameServerConfig cfg;
  cfg.max_result_tries = 3;
  Fixture f(std::move(cfg), true);
  f.lobby->fail_results = 2;  // answers 503 twice, then 200
  CHECK(f.status(f.call("POST", "/rooms", room_body("t1", {1, 2}, "{\"end_after\":120}"))) == 201);
  f.add_player("t1", 1, 1);
  f.add_player("t1", 2, 2);
  f.run(9'000'000);
  CHECK(f.lobby->results.size() == 3 && f.gs->stats().results_sent == 1 && f.gs->stats().results_dropped == 0);

  f.lobby->fail_results = 100;  // never
  CHECK(f.status(f.call("POST", "/rooms", room_body("t2", {3, 4}, "{\"end_after\":120}"))) == 201);
  f.add_player("t2", 3, 3);
  f.add_player("t2", 4, 4);
  f.run(12'000'000);
  CHECK(f.gs->stats().results_dropped == 1);
}

void nobody_showing_up_and_everybody_leaving() {
  gameserver::GameServerConfig cfg;
  cfg.match.join_timeout = 3'000'000;
  Fixture f(std::move(cfg), true);
  CHECK(f.status(f.call("POST", "/rooms", room_body("n1", {1, 2}))) == 201);
  f.add_player("n1", 1, 1);  // the second never comes
  f.run(5'000'000);
  CHECK(f.gs->match_count() == 0);
  CHECK(f.lobby->results.size() == 1);
  auto v = json::parse(f.lobby->results[0].second);
  CHECK(*v->find("outcome")->as_string() == "no_show");
  CHECK(!*v->find("joined")->as_array()->at(1).as_bool() && *v->find("joined")->as_array()->at(0).as_bool());

  CHECK(f.status(f.call("POST", "/rooms", room_body("n2", {3, 4}))) == 201);
  f.add_player("n2", 3, 3);
  f.add_player("n2", 4, 4);
  f.run(3'000'000);
  f.players[1].session->disconnect();  // n1's lone player is players[0]
  f.players[2].session->disconnect();
  f.run(2'000'000);
  CHECK(f.gs->match_count() == 0);
  CHECK(f.lobby->results.size() == 2);
  CHECK(*json::parse(f.lobby->results[1].second)->find("outcome")->as_string() == "abandoned");
}

}  // namespace

int main() {
  a_short_secret_or_no_factory_is_refused();
  creating_a_game_and_playing_it();
  many_matches_at_once_and_capacity();
  the_control_api_needs_the_secret_and_checks_its_input();
  only_the_right_token_gets_a_seat();
  bad_joiners_are_closed();
  a_finished_match_reports_its_result();
  registration_is_repeated_and_authenticated();
  results_are_retried_and_then_dropped();
  nobody_showing_up_and_everybody_leaving();
  std::puts("gameserver_test: ok");
  return 0;
}
