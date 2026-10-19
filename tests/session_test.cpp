// Whole matches over the simulated network: a server session and client
// sessions, each client with its own copy of the game. Whatever the link does,
// every state a client has confirmed must be the one a straight replay of the
// server's canonical inputs gives.

#include <cstdio>

#include "check.hpp"
#include "match.hpp"

namespace {

using session::ClientSession;
using transport::LinkConditions;

constexpr Micros ms = 1000;

// Every checkpoint of every client agrees with the replay. Returns how many were checked.
std::size_t check_convergence(const Match& m) {
  const auto ref = m.reference_hashes();
  std::size_t n = 0;
  for (const auto& p : m.players_) {
    for (const auto& c : p.session->checkpoints()) {
      CHECK(c.frame < ref.size());
      CHECK(c.hash == ref[c.frame]);
      ++n;
    }
  }
  return n;
}

void all_running(const Match& m) {
  CHECK(m.server->state() == session::ServerSession::State::Running);
  for (const auto& p : m.players_) {
    CHECK(p.session->state() == ClientSession::State::Running);
  }
}

void conditions(Match& m, const LinkConditions& c) { m.net.set_default_conditions(c); }

void clean_link() {
  Match m(1, 2);
  conditions(m, {.delay = 10 * ms});
  m.connect_all();
  m.run(10'000 * ms);
  all_running(m);
  CHECK(m.server->next_frame() > 500);
  CHECK(check_convergence(m) >= 2 * 10);
  // Inputs arrive in time: the server hardly ever has to guess or drop one.
  CHECK(m.server->stats().frames_missing <= 4);
  CHECK(m.server->stats().inputs_invalid == 0 && m.server->stats().messages_malformed == 0);
  for (const auto& p : m.players_) {
    // 20 ms of round trip: the rule says 1 frame, and a rollback is at most a couple deep.
    CHECK(p.session->input_delay() == 1);
    CHECK(p.session->rollback().stats().max_depth <= 3);
    CHECK(p.session->rollback().frame() + 4 >= m.server->next_frame());
  }
}

void lossy_jittery_link() {
  for (std::uint64_t seed = 1; seed <= 6; ++seed) {
    Match m(seed, 2 + seed % 3);
    conditions(m, {.drop = 0.10, .delay = 30 * ms, .jitter = 20 * ms, .reorder = 0.05,
                   .reorder_delay = 40 * ms, .duplicate = 0.05});
    m.connect_all();
    m.run(15'000 * ms);
    all_running(m);
    CHECK(m.server->next_frame() > 500);
    CHECK(check_convergence(m) >= m.players_n * 10);
    for (const auto& p : m.players_) {
      CHECK(p.session->rollback().ok());
      CHECK(p.session->rollback().stats().max_depth <= 16);
      CHECK(p.session->rollback().stats().rollbacks > 0);  // it did have to roll back
    }
  }
}

void heavy_loss() {
  Match m(9, 2);
  conditions(m, {.drop = 0.30, .delay = 40 * ms, .jitter = 30 * ms, .duplicate = 0.05});
  m.connect_all();
  m.run(20'000 * ms);
  all_running(m);
  CHECK(check_convergence(m) >= 10);
}

void different_players_different_links() {
  Match m(3, 3);
  const Address s = m.server_addr;
  const LinkConditions near{.delay = 5 * ms}, far{.delay = 70 * ms, .jitter = 10 * ms};
  for (std::size_t i = 0; i < 3; ++i) {
    const auto a = Address::v4(10, 1, 0, static_cast<std::uint8_t>(i + 1), 5000);
    const auto& c = i == 2 ? far : near;
    m.net.set_conditions(a, s, c);
    m.net.set_conditions(s, a, c);
  }
  m.connect_all();
  m.run(15'000 * ms);
  all_running(m);
  CHECK(check_convergence(m) >= 3 * 10);
  // The far player's round trip is 140 ms: 70 ms to the server is 4.2 frames, plus jitter.
  CHECK(m.players_[2].session->input_delay() >= 4);
  CHECK(m.players_[0].session->input_delay() == 1);
}

void clocks_far_apart() {
  Match m(4, 2);
  conditions(m, {.delay = 20 * ms, .jitter = 5 * ms});
  m.players_[0].skew = 4'000'000'000;
  m.players_[1].skew = -2'500'000'000;
  m.connect_all();
  m.run(10'000 * ms);
  all_running(m);
  CHECK(check_convergence(m) >= 2 * 10);
  for (const auto& p : m.players_) CHECK(p.session->rollback().frame() + 6 >= m.server->next_frame());
}

void a_player_who_leaves() {
  Match m(5, 3);
  conditions(m, {.delay = 15 * ms, .jitter = 5 * ms});
  m.connect_all();
  m.run(5'000 * ms);
  all_running(m);
  const auto leaver = m.players_[1].session->player();  // seats go in order of arrival
  m.players_[1].session->disconnect();
  const Frame at_leave = m.server->next_frame();
  m.run(10'000 * ms);
  CHECK(!m.server->seat_connected(leaver));
  CHECK(m.server->seated() == 2);
  CHECK(m.server->next_frame() > at_leave + 400);  // the others were not held up
  CHECK(check_convergence(m) >= 2 * 10);
  CHECK(m.players_[0].session->state() == ClientSession::State::Running);
}

void a_player_who_goes_silent() {
  Match m(6, 3);
  conditions(m, {.delay = 15 * ms});
  m.connect_all();
  m.run(5'000 * ms);
  m.players_[2].silent = true;  // connected, but sends nothing
  const Frame at = m.server->next_frame();
  m.run(8'000 * ms);
  // The server keeps finalising on the clock (60 frames a second), the others play on.
  CHECK(m.server->next_frame() > at + 400);
  CHECK(m.server->stats().frames_missing > 300);
  CHECK(check_convergence(m) >= 2 * 10);
  CHECK(m.players_[0].session->state() == ClientSession::State::Running);
  CHECK(m.players_[0].session->rollback().frame() + 8 >= m.server->next_frame());
}

void extra_client_gets_no_seat() {
  Match m(7, 2);
  conditions(m, {.delay = 10 * ms});
  m.connect_all();
  m.run(2'000 * ms);
  // A third client connects to a full match.
  auto link = m.net.make_link(Address::v4(10, 1, 0, 9, 5000));
  ToyGame g(2);
  ClientSession late(*link, m.server_addr, g, [](Frame, std::span<std::uint8_t> o) { o[0] = 1; o[1] = 0; });
  late.connect(m.net.now());
  for (int i = 0; i < 3000; ++i) {
    late.update(m.net.now());
    m.step();
  }
  CHECK(late.state() == ClientSession::State::Ended);
  all_running(m);
  CHECK(m.server->seated() == 2);
}

void the_game_that_does_not_fit_is_refused() {
  // 20 players of 100 bytes each is 2000 bytes a frame; the protocol carries 1100.
  struct Big final : session::Simulation {
    std::size_t player_count() const override { return 20; }
    std::size_t input_size() const override { return 100; }
    void step(Frame, std::span<const std::uint8_t>) override {}
    void snapshot(std::vector<std::uint8_t>& o) const override { o.clear(); }
    bool restore(std::span<const std::uint8_t>) override { return true; }
    std::uint64_t hash() const override { return 0; }
  } big;
  transport::SimNetwork net(1);
  auto link = net.make_link(Address::v4(10, 0, 0, 1, 7000));
  session::ServerSession server(*link, big);
  CHECK(server.state() == session::ServerSession::State::Finished);
  ClientSession c(*link, Address::v4(10, 0, 0, 2, 7000), big, [](Frame, std::span<std::uint8_t>) {});
  c.connect(0);
  CHECK(c.state() == ClientSession::State::Ended);
}

}  // namespace

int main() {
  clean_link();
  lossy_jittery_link();
  heavy_loss();
  different_players_different_links();
  clocks_far_apart();
  a_player_who_leaves();
  a_player_who_goes_silent();
  extra_client_gets_no_seat();
  the_game_that_does_not_fit_is_refused();
  std::puts("session_test: ok");
  return 0;
}
