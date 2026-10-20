// A bot that plays the toy game through the lobby, end to end: finds or creates a
// room, takes a seat, readies, gets its token, joins the game server over UDP and
// plays until the match ends. A demo of the client side, and the end-to-end test.
//
//   toy_bot --lobby HOST:PORT --room NAME [--create SEATS] [--frames N] [--name BOT] [--timeout SECONDS]
//
// Prints "RESULT outcome=<outcome> frames=<n>" and exits 0 if the match completed.

#include <chrono>
#include <cstring>
#include <thread>

#include "npk/app/env.hpp"
#include "npk/http/client.hpp"
#include "npk/json/json.hpp"
#include "npk/session/client_session.hpp"
#include "npk/transport/udp_socket.hpp"
#include "toy_game.hpp"

using namespace npk;

namespace {

struct Args {
  std::string lobby, room, name = "bot";
  long create = 0, frames = 300, timeout = 60;
};

bool parse(int argc, char** argv, Args& a) {
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i], v = argv[i + 1];
    if (k == "--lobby") a.lobby = v;
    else if (k == "--room") a.room = v;
    else if (k == "--name") a.name = v;
    else if (k == "--create") a.create = std::atol(v.c_str());
    else if (k == "--frames") a.frames = std::atol(v.c_str());
    else if (k == "--timeout") a.timeout = std::atol(v.c_str());
    else return false;
  }
  return !a.lobby.empty() && !a.room.empty() && a.create >= 0 && a.create <= 4 && a.frames > 0 && a.timeout > 0;
}

struct Lobby {
  transport::Address addr;
  std::string host;
  http::Client client;

  // One blocking request; the body parsed if it is JSON.
  std::pair<int, json::Value> call(const std::string& method, const std::string& path, const std::string& body = "",
                                   const std::string& bearer = "") {
    http::OutgoingRequest q;
    q.method = method;
    q.path = path;
    q.body = body;
    if (!bearer.empty()) q.headers.emplace_back("Authorization", "Bearer " + bearer);
    std::optional<http::Result> got;
    client.request(addr, host, q, transport::steady_now_us(), [&](const http::Result& r) { got = r; });
    while (!got && !app::stop_requested()) {
      client.update(transport::steady_now_us());
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!got || !got->ok) return {-1, {}};
    auto v = json::parse(got->body);
    return {got->status, v ? std::move(*v) : json::Value()};
  }
};

[[noreturn]] void fail(const char* why) {
  std::fprintf(stderr, "bot: %s\n", why);
  std::exit(1);
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse(argc, argv, a)) {
    std::fprintf(stderr, "usage: toy_bot --lobby HOST:PORT --room NAME [--create SEATS] [--frames N] [--name BOT] [--timeout S]\n");
    return 2;
  }
  app::install_signal_handlers();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(a.timeout);
  auto expired = [&] { return std::chrono::steady_clock::now() > deadline || app::stop_requested(); };
  auto nap = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };

  const auto hp = http::parse_host_port(a.lobby, 80);
  const auto addr = hp ? http::resolve(hp->host, hp->port) : std::nullopt;
  if (!addr) fail("cannot resolve the lobby");
  Lobby lobby{*addr, a.lobby, http::Client{}};

  // A game server must be registered before a room can start.
  for (;;) {
    auto [st, v] = lobby.call("GET", "/api/status");
    if (st == 200 && v.find("game_servers") && v.find("game_servers")->as_int().value_or(0) > 0) break;
    if (expired()) fail("no game server registered");
    nap(200);
  }

  std::string room_id;
  if (a.create > 0) {
    json::Value body, options;
    options.set("end_after", static_cast<std::int64_t>(a.frames));
    body.set("name", a.room);
    body.set("game", "toy");
    body.set("seats", static_cast<std::int64_t>(a.create));
    body.set("options", options);
    auto [st, v] = lobby.call("POST", "/api/rooms", json::dump(body));
    if (st != 201) fail("cannot create the room");
    room_id = *v.find("id")->as_string();
  } else {
    while (room_id.empty()) {
      auto [st, v] = lobby.call("GET", "/api/rooms");
      if (st == 200) {
        for (const auto& r : *v.find("rooms")->as_array()) {
          if (*r.find("name")->as_string() == a.room && *r.find("state")->as_string() == "open") room_id = *r.find("id")->as_string();
        }
      }
      if (room_id.empty() && expired()) fail("room not found");
      if (room_id.empty()) nap(200);
    }
  }

  json::Value jb;
  jb.set("name", a.name);
  auto [jst, joined] = lobby.call("POST", "/api/rooms/" + room_id + "/join", json::dump(jb));
  if (jst != 200) fail("cannot take a seat");
  const std::string secret = *joined.find("secret")->as_string();
  const std::string path = "/api/rooms/" + room_id;
  json::Value rb;
  rb.set("ready", true);
  if (lobby.call("POST", path + "/ready", json::dump(rb), secret).first != 200) fail("cannot ready");

  std::string token, udp;
  std::int64_t seats = 0;
  for (;;) {
    auto [st, v] = lobby.call("POST", path + "/me", "", secret);
    if (st == 200 && *v.find("state")->as_string() == "playing") {
      token = *v.find("token")->as_string();
      udp = *v.find("server")->find("udp")->as_string();
      break;
    }
    if (expired()) fail("the match did not start");
    nap(200);
  }
  if (auto [st, v] = lobby.call("GET", path); st == 200) seats = *v.find("seat_count")->as_int();

  // Play.
  const auto server_hp = http::parse_host_port(udp, 0);
  const auto server = server_hp ? http::resolve(server_hp->host, server_hp->port) : std::nullopt;
  if (!server || seats < 1) fail("cannot reach the game server");
  std::string err;
  auto sock = transport::UdpSocket::bind(server->is_v4() ? transport::Address::any_v4(0) : transport::Address::any_v6(0), &err);
  if (!sock) fail("cannot open a UDP socket");
  ToyGame game(static_cast<std::size_t>(seats));
  session::ClientSessionConfig cc;
  session::msg::Join j;
  j.room = room_id;
  for (std::size_t i = 0; i < session::msg::kTokenSize; ++i) j.token[i] = static_cast<std::uint8_t>(std::stoi(token.substr(2 * i, 2), nullptr, 16));
  cc.join = j;
  const auto script = toy_inputs(std::hash<std::string>{}(a.name), 1, 20000);
  session::ClientSession session(*sock, *server, game, [&](session::Frame w, std::span<std::uint8_t> out) {
    out[0] = script[(w % 20000) * 2];
    out[1] = script[(w % 20000) * 2 + 1];
  }, cc);
  session.connect(transport::steady_now_us());
  while (session.state() != session::ClientSession::State::Ended && !expired()) {
    session.update(transport::steady_now_us());
    sock->wait_readable(2);
  }
  if (session.state() != session::ClientSession::State::Ended) fail("the match did not end");

  // The result as the lobby has it.
  for (;;) {
    auto [st, v] = lobby.call("GET", path);
    if (st == 200 && *v.find("state")->as_string() == "finished") {
      const auto* result = v.find("result");
      const std::string outcome = *result->find("outcome")->as_string();
      const std::int64_t frames = result->find("frames") ? result->find("frames")->as_int().value_or(-1) : -1;
      std::printf("RESULT outcome=%s frames=%lld\n", outcome.c_str(), static_cast<long long>(frames));
      return outcome == "completed" ? 0 : 1;
    }
    if (expired()) fail("the lobby has no result");
    nap(200);
  }
}
