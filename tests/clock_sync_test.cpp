// Round-trip and clock-offset estimation: the estimator, and the ping/pong
// exchange over the simulated network with the two clocks set apart.
#include <cstdlib>
#include <vector>

#include "check.hpp"
#include "npk/transport/clock_sync.hpp"
#include "world.hpp"

using namespace npk::transport;

namespace {

using Bytes = std::vector<std::uint8_t>;

void estimator_prefers_the_cleanest_recent_sample() {
  ClockSync c;
  CHECK(!c.has_estimate());
  c.add(100'000, 5'000);    // queued: rtt high, offset disturbed
  c.add(40'000, 1'000);     // the clean one
  c.add(70'000, -3'000);
  CHECK(c.has_estimate() && c.sample_count() == 3);
  CHECK(c.min_rtt() == 40'000 && c.offset() == 1'000);
  CHECK(c.to_local(11'000) == 10'000 && c.to_peer(10'000) == 11'000);
  // The clean sample ages out of the window once 16 newer ones have come.
  for (int i = 0; i < 16; ++i) c.add(60'000 + i, 2'000);
  CHECK(c.min_rtt() == 60'000 && c.offset() == 2'000);
  // The smoothed round trip follows steady input and reports its spread.
  ClockSync s;
  for (int i = 0; i < 100; ++i) s.add(50'000, 0);
  CHECK(s.rtt() == 50'000 && s.jitter() < 100);
  for (int i = 0; i < 100; ++i) s.add(i % 2 ? 40'000 : 60'000, 0);
  CHECK(s.rtt() >= 45'000 && s.rtt() <= 55'000 && s.jitter() >= 5'000);
}

// Runs a connected client against the server with the client's clock `skew` ahead.
struct Pair {
  Pair(std::uint64_t seed, Micros skew, const LinkConditions& up, const LinkConditions& down) : w(seed) {
    w.client_skew = skew;
    w.net.set_conditions(client_addr(0), kServer, up);
    w.net.set_conditions(kServer, client_addr(0), down);
    c = &w.add_client(0);
    c->connect(w.net.now() + skew);
  }
  World w;
  Client* c;
};

void clocks_far_apart_are_found_on_a_clean_link() {
  for (const Micros skew : {Micros{5'000'000'000}, Micros{-7'000'000'000}, Micros{0}}) {
    Pair p(1, skew, {.delay = 25'000}, {.delay = 25'000});
    p.w.run(3'000'000);
    CHECK(p.c->state() == Client::State::Connected);
    const auto clock = *p.c->clock();
    // The server's clock reads the client's minus skew. Peer minus local = -skew.
    CHECK(std::llabs(clock.offset() - (-skew)) <= 1'000);  // within one 1 ms tick
    CHECK(clock.min_rtt() >= 50'000 && clock.min_rtt() <= 52'000);
    CHECK(clock.rtt() >= 50'000 && clock.rtt() <= 53'000);
    // The server sees the mirror image.
    const auto srv = *p.w.server->clock(p.c->connection_id());
    CHECK(srv.has_estimate() && std::llabs(srv.offset() - skew) <= 1'000);
    CHECK(srv.min_rtt() >= 50'000 && srv.min_rtt() <= 52'000);
    CHECK(p.w.server->stats().amplification_blocked == 0);
  }
}

void the_estimate_survives_loss_and_jitter() {
  LinkConditions bad;
  bad.drop = 0.2;
  bad.delay = 30'000;
  bad.jitter = 30'000;
  bad.reorder = 0.1;
  bad.reorder_delay = 70'000;
  bad.duplicate = 0.1;
  for (std::uint64_t seed = 1; seed <= 10; ++seed) {
    const Micros skew = 123'456'789'012LL * (seed % 2 ? 1 : -1);
    Pair p(seed, skew, bad, bad);
    p.w.run(20'000'000);
    CHECK(p.c->state() == Client::State::Connected);
    const auto clock = *p.c->clock();
    CHECK(clock.sample_count() >= 5);
    // The path takes at least 60 ms there and back. A sample whose round trip is
    // `excess` over that has at most `excess` of queueing split between the two
    // directions, so its offset is wrong by at most excess / 2 (plus the 1 ms tick).
    // The lowest-rtt sample keeps that small.
    const Micros excess = clock.min_rtt() - 60'000;
    CHECK(excess >= -2'000 && excess <= 25'000);
    CHECK(std::llabs(clock.offset() + skew) <= excess / 2 + 2'000);
    CHECK(clock.rtt() > clock.min_rtt() - 1);
  }
}

void unequal_directions_are_the_one_thing_pings_cannot_see() {
  // 10 ms up, 40 ms down: the offset comes out half the difference off, by construction.
  Pair p(3, 1'000'000'000, {.delay = 10'000}, {.delay = 40'000});
  p.w.run(5'000'000);
  const auto clock = *p.c->clock();
  const Micros truth = -1'000'000'000;          // peer minus local
  const Micros bias = (10'000 - 40'000) / 2;    // (up - down) / 2
  CHECK(std::llabs(clock.offset() - (truth + bias)) <= 1'500);
  CHECK(clock.min_rtt() >= 50'000 && clock.min_rtt() <= 52'000);  // the round trip is still right
}

void the_handshake_gives_a_first_estimate() {
  Pair p(4, 777'000'000, {.delay = 20'000}, {.delay = 20'000});
  while (p.c->state() != Client::State::Connected) p.w.step();
  const auto clock = *p.c->clock();
  CHECK(clock.sample_count() == 1);  // from the Init/Challenge round trip and the Accept's timestamp
  CHECK(std::llabs(clock.offset() + 777'000'000) <= 3'000);
  CHECK(clock.min_rtt() >= 40'000 && clock.min_rtt() <= 43'000);
}

struct Rig {
  Rig() : w(9) {
    w.net.set_default_conditions({.delay = 10'000});
    c = &w.add_client(0);
    c->connect(0);
    w.run(1'000'000);
    CHECK(c->state() == Client::State::Connected);
    id = c->connection_id();
    w.clients_silent = true;
    link = w.clients[0].link.get();
  }
  World w;
  Client* c;
  ConnectionId id;
  SimLink* link;
};

void pongs_must_answer_our_pings() {
  Rig r;
  const auto before = r.w.server->clock(r.id)->sample_count();
  // The server pinged us; we are silent, so those pings had their pongs sent by nobody.
  // Forge answers: an id never sent, and a plausible id with the wrong echoed time.
  std::uint16_t seq = 500;
  auto forge = [&](std::uint32_t ping_id, std::uint64_t t1) {
    PacketBuilder b(Header{PacketType::Data, r.id, seq++});
    b.add_pong(ping_id, t1, 1, 2);
    r.link->send(kServer, b.bytes());
    r.w.run(3'000);
  };
  for (std::uint32_t id = 0; id < 40; ++id) forge(id, 12345);
  forge(0xffffffff, 1);
  CHECK(r.w.server->clock(r.id)->sample_count() == before);
  CHECK(r.w.server->connection_count() == 1);
}

void a_flood_of_pings_draws_only_a_few_pongs() {
  Rig r;
  Address from;
  Bytes buf(2048);
  while (r.link->receive(from, buf)) {
  }
  PacketBuilder b(Header{PacketType::Data, r.id, 600});
  int added = 0;
  while (b.add_ping(static_cast<std::uint32_t>(added), 1000)) ++added;
  CHECK(added > 60);
  const std::size_t request = b.bytes().size();
  r.link->send(kServer, b.bytes());
  r.w.run(100'000);
  int pongs = 0;
  std::size_t reply_bytes = 0;
  while (auto n = r.link->receive(from, buf)) {
    reply_bytes += *n;
    Packet p;
    if (parse_packet({buf.data(), *n}, p) != ParseError::None) continue;
    FrameReader fr(p.body);
    Frame f;
    while (!fr.done() && fr.next(f) == ParseError::None) pongs += f.kind == FrameKind::Pong;
  }
  CHECK(pongs >= 1 && pongs <= static_cast<int>(Connection::kMaxPongsPerFlush));
  CHECK(reply_bytes < request / 4);
}

void a_pong_counts_once() {
  // Run a real exchange, then replay a captured pong: the estimate must not move.
  World w(11);
  w.net.set_default_conditions({.delay = 10'000, .duplicate = 1});  // every packet arrives twice
  Client& c = w.add_client(0);
  c.connect(0);
  w.run(3'000'000);
  const auto n = w.server->clock(c.connection_id())->sample_count();
  // Each ping the server sent was answered once even though every packet was duplicated:
  // samples = pings sent so far (give or take the in-flight one), not twice that.
  CHECK(n >= 5 && n <= 12);
}

}  // namespace

int main() {
  estimator_prefers_the_cleanest_recent_sample();
  clocks_far_apart_are_found_on_a_clean_link();
  the_estimate_survives_loss_and_jitter();
  unequal_directions_are_the_one_thing_pings_cannot_see();
  the_handshake_gives_a_first_estimate();
  pongs_must_answer_our_pings();
  a_flood_of_pings_draws_only_a_few_pongs();
  a_pong_counts_once();
  return 0;
}
