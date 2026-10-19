// Desync detection and repair, and hostile peers. A client whose game goes
// wrong (here: a bug that changes the state in a way no input explains) is
// found by its checkpoint hashes and given the server's state. A client or a
// server that sends garbage must not hurt the other side.

#include <cstdio>
#include <random>

#include "check.hpp"
#include "match.hpp"

namespace {

using session::ClientSession;
using transport::LinkConditions;
constexpr Micros ms = 1000;

// Checkpoints of frame >= from that disagree with the replay, and how many were compared.
struct Agreement {
  std::size_t checked = 0, wrong = 0;
};
Agreement agreement(const Match& m, std::size_t player, Frame from) {
  const auto ref = m.reference_hashes();
  Agreement a;
  for (const auto& c : m.players_[player].session->checkpoints()) {
    if (c.frame < from || c.frame >= ref.size()) continue;
    ++a.checked;
    a.wrong += c.hash != ref[c.frame];
  }
  return a;
}

void a_desynced_client_is_repaired() {
  for (std::uint64_t seed = 1; seed <= 3; ++seed) {
    Match m(seed, 3);
    m.net.set_default_conditions(seed == 1 ? LinkConditions{.delay = 15 * ms}
                                           : LinkConditions{.drop = 0.1, .delay = 30 * ms, .jitter = 15 * ms,
                                                            .reorder = 0.05, .reorder_delay = 30 * ms, .duplicate = 0.05});
    m.players_[1].game->bug_at_frame = 100;  // once
    m.connect_all();
    m.run(15'000 * ms);
    CHECK(m.server->stats().desyncs >= 1);
    CHECK(m.server->stats().resyncs_sent >= 1 && m.server->stats().resyncs_sent <= 3);
    CHECK(m.players_[1].session->stats().resyncs >= 1);
    CHECK(m.players_[1].session->state() == ClientSession::State::Running);
    // After the repair its checkpoints agree with the server's again, for the whole rest of the match.
    const Frame resumed = m.server->next_frame() - 400;
    const auto bad = agreement(m, 1, resumed);
    CHECK(bad.checked >= 8 && bad.wrong == 0);
    for (std::size_t p : {0u, 2u}) {
      const auto ok = agreement(m, p, 0);
      CHECK(ok.checked >= 8 && ok.wrong == 0);
      CHECK(m.players_[p].session->stats().resyncs == 0);
    }
    CHECK(m.server->stats().clients_dropped == 0);
  }
}

void a_client_that_cannot_stay_repaired_is_dropped() {
  Match m(5, 2);
  m.net.set_default_conditions({.delay = 10 * ms});
  m.players_[0].game->bug_every = 45;  // never right for long
  m.connect_all();
  m.run(30'000 * ms);
  CHECK(m.server->stats().clients_dropped == 1);
  CHECK(m.server->stats().resyncs_sent <= 6);
  CHECK(m.players_[0].session->state() == ClientSession::State::Ended);
  CHECK(m.players_[1].session->state() == ClientSession::State::Running);
  CHECK(m.server->seated() == 1);
  const auto ok = agreement(m, 1, 0);
  CHECK(ok.checked >= 20 && ok.wrong == 0);
}

void a_client_far_behind_is_caught_up() {
  Match m(6, 2);
  m.net.set_default_conditions({.delay = 20 * ms});
  m.connect_all();
  m.run(3'000 * ms);
  m.players_[1].silent = true;  // frozen for 6 seconds: more frames than the server keeps
  m.run(6'000 * ms);
  m.players_[1].silent = false;
  m.run(5'000 * ms);
  CHECK(m.players_[1].session->state() == ClientSession::State::Running);
  CHECK(m.players_[1].session->stats().resyncs >= 1);
  CHECK(m.server->stats().desyncs == 0);  // not a desync, a catch-up
  CHECK(m.players_[1].session->rollback().frame() + 8 >= m.server->next_frame());
  const auto ok = agreement(m, 1, m.server->next_frame() - 150);
  CHECK(ok.checked >= 3 && ok.wrong == 0);
}

// A seated client that is not a game client, and sends whatever it likes.
void a_hostile_client() {
  Match m(7, 2, {}, {}, 1);  // seat 1 is for the honest session; the other is ours
  m.net.set_default_conditions({.delay = 10 * ms});
  auto link = m.net.make_link(Address::v4(10, 1, 0, 50, 5000));
  transport::Client evil(*link, m.server_addr, {.seed = 99});
  evil.connect(m.net.now());
  m.players_[0].session->connect(m.net.now());
  std::mt19937_64 rng(1234);
  auto garbage = [&](std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (auto& b : v) b = static_cast<std::uint8_t>(rng());
    return v;
  };
  for (int i = 0; i < 20000; ++i) {
    evil.update(m.net.now());
    while (evil.poll_message()) {}
    if (evil.state() == transport::Client::State::Connected && i % 3 == 0) {
      std::vector<std::uint8_t> v;
      switch (rng() % 8) {
        case 0: evil.send_unreliable(garbage(rng() % 300)); break;
        case 1: evil.send_reliable(garbage(rng() % 300)); break;
        case 2: {  // an Inputs message with an invalid input (direction 9)
          std::vector<std::uint8_t> in{9, 9};
          session::msg::encode_inputs(static_cast<Frame>(rng() % 100), static_cast<Frame>(rng()), 1, in, v);
          evil.send_unreliable(v);
          break;
        }
        case 3: {  // a frame far in the future, and one in the past
          std::vector<std::uint8_t> in{1, 0};
          session::msg::encode_inputs(static_cast<Frame>(rng()), 0xffffffffu, 1, in, v);
          evil.send_unreliable(v);
          break;
        }
        case 4:  // a hash for a frame the server never had, and a wrong one for one it did
          session::msg::encode_hash({.frame = static_cast<Frame>(rng() % 3000 / 30 * 30), .hash = rng()}, v);
          evil.send_reliable(v);
          break;
        case 5:  // messages only a server sends
          session::msg::encode_resync(30, garbage(40), v);
          evil.send_reliable(v);
          break;
        case 6:
          session::msg::encode_frames(0, 1, garbage(4), v);
          evil.send_unreliable(v);
          break;
        default: {  // truncated and oversize variants of valid messages
          std::vector<std::uint8_t> in(static_cast<std::size_t>(rng() % 80), 1);
          session::msg::encode_inputs(0, 0, rng() % 40, in, v);
          evil.send_unreliable(v);
        }
      }
    }
    m.step();
  }
  CHECK(m.server->state() == session::ServerSession::State::Running);
  CHECK(m.players_[0].session->state() == ClientSession::State::Running);
  CHECK(m.players_[0].session->rollback().ok());
  const auto ok = agreement(m, 0, 0);
  CHECK(ok.checked >= 20 && ok.wrong == 0);
  // The evil client is dropped for its bad messages (or at least ignored and never harmful).
  const auto& st = m.server->stats();
  CHECK(st.messages_malformed + st.inputs_invalid + st.desyncs > 0);
  CHECK(st.clients_dropped <= 1);
}

// A "server" that sends a client garbage: the client may end, never crash.
void a_hostile_server() {
  struct Bad {
    const char* name;
    std::function<void(transport::Server&, transport::ConnectionId, std::mt19937_64&)> act;
  };
  auto start = [](session::msg::Start s) {
    std::vector<std::uint8_t> v;
    session::msg::encode_start(s, v);
    return v;
  };
  const session::msg::Start good{.player = 0, .players = 2, .input_size = 2, .tick_rate = 60, .hash_interval = 30,
                                 .start_time = 100'000};
  std::vector<Bad> cases;
  cases.push_back({"wrong player count", [&](auto& s, auto id, auto&) {
    auto b = good; b.players = 3; s.send_reliable(id, start(b)); }});
  cases.push_back({"wrong input size", [&](auto& s, auto id, auto&) {
    auto b = good; b.input_size = 9; s.send_reliable(id, start(b)); }});
  cases.push_back({"seat out of range", [&](auto& s, auto id, auto&) {
    auto b = good; b.player = 2; s.send_reliable(id, start(b)); }});
  cases.push_back({"absurd tick rate", [&](auto& s, auto id, auto&) {
    auto b = good; b.tick_rate = 60000; s.send_reliable(id, start(b)); }});
  cases.push_back({"other hash interval", [&](auto& s, auto id, auto&) {
    auto b = good; b.hash_interval = 7; s.send_reliable(id, start(b)); }});
  cases.push_back({"invalid canonical input", [&](auto& s, auto id, auto&) {
    s.send_reliable(id, start(good));
    std::vector<std::uint8_t> v;
    session::msg::encode_frames(0, 1, std::vector<std::uint8_t>{1, 0, 9, 9}, v);
    s.send_unreliable(id, v); }});
  cases.push_back({"resync with a bad snapshot", [&](auto& s, auto id, auto&) {
    s.send_reliable(id, start(good));
    std::vector<std::uint8_t> v;
    session::msg::encode_resync(30, std::vector<std::uint8_t>(7, 1), v);
    s.send_reliable(id, v); }});
  cases.push_back({"resync from the far future", [&](auto& s, auto id, auto&) {
    s.send_reliable(id, start(good));
    ToyGame g(2);
    std::vector<std::uint8_t> snap, v;
    g.snapshot(snap);
    session::msg::encode_resync(4'000'000'000u, snap, v);
    s.send_reliable(id, v); }});
  cases.push_back({"resync before start", [&](auto& s, auto id, auto&) {
    ToyGame g(2);
    std::vector<std::uint8_t> snap, v;
    g.snapshot(snap);
    session::msg::encode_resync(0, snap, v);
    s.send_reliable(id, v); }});
  cases.push_back({"garbage after a good start", [&](auto& s, auto id, auto& rng) {
    s.send_reliable(id, start(good));
    for (int i = 0; i < 200; ++i) {
      std::vector<std::uint8_t> v(rng() % 200);
      for (auto& b : v) b = static_cast<std::uint8_t>(rng());
      if (!v.empty() && i % 2) v[0] = static_cast<std::uint8_t>(rng() % 3 ? 1 + rng() % 2 : 16 + rng() % 3);
      (i % 3 ? s.send_unreliable(id, v) : s.send_reliable(id, v));
    } }});
  for (const Bad& c : cases) {
    transport::SimNetwork net(11);
    net.set_default_conditions({.delay = 5 * ms});
    auto slink = net.make_link(Address::v4(10, 0, 0, 1, 7000));
    auto clink = net.make_link(Address::v4(10, 1, 0, 1, 5000));
    transport::Server server(*slink, {.seed = 5});
    ToyGame game(2);
    ClientSession client(*clink, Address::v4(10, 0, 0, 1, 7000), game,
                         [](Frame, std::span<std::uint8_t> o) { o[0] = 1; o[1] = 0; }, {.transport = {.seed = 6}});
    client.connect(0);
    std::mt19937_64 rng(77);
    transport::ConnectionId id = 0;
    bool acted = false;
    for (int i = 0; i < 3000; ++i) {
      client.update(net.now());
      server.update(net.now());
      while (auto e = server.poll_event()) {
        if (e->type == transport::ServerEvent::Type::Connected) id = e->id;
      }
      while (server.poll_message()) {}
      if (id && !acted) {
        acted = true;
        c.act(server, id, rng);
      }
      net.advance(1000);
    }
    CHECK(acted);
    const bool refused = client.state() == ClientSession::State::Ended &&
                         client.end_reason() == transport::DisconnectReason::ProtocolError;
    const bool survives = client.state() == ClientSession::State::Running && client.rollback().ok();
    if (!refused && !survives) std::fprintf(stderr, "case \"%s\": state %d\n", c.name, static_cast<int>(client.state()));
    CHECK(refused || survives);
    // The ones that are clearly protocol violations must end the session.
    const std::string name = c.name;
    if (name != "garbage after a good start") CHECK(refused);
  }
}

}  // namespace

int main() {
  a_desynced_client_is_repaired();
  a_client_that_cannot_stay_repaired_is_dropped();
  a_client_far_behind_is_caught_up();
  a_hostile_client();
  a_hostile_server();
  std::puts("desync_test: ok");
  return 0;
}
