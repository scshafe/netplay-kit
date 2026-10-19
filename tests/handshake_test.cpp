// The handshake over the simulated network: it connects through loss, never
// amplifies, keeps no state for unverified sources, and limits what it answers.
#include <memory>
#include <random>
#include <vector>

#include "check.hpp"
#include "npk/transport/client.hpp"
#include "npk/transport/server.hpp"
#include "npk/transport/sim_link.hpp"

using namespace npk::transport;

namespace {

const Address kServer = Address::v4(10, 0, 0, 1, 7000);

Address client_addr(int i) { return Address::v4(10, 1, static_cast<std::uint8_t>(i / 250), static_cast<std::uint8_t>(i % 250 + 1), 5000); }

struct Peer {
  std::unique_ptr<SimLink> link;
  std::unique_ptr<Client> client;
};

struct World {
  explicit World(std::uint64_t seed, const ServerConfig& cfg = {}) : net(seed) {
    server_link = net.make_link(kServer);
    ServerConfig c = cfg;
    if (c.seed == 0) c.seed = seed * 7919 + 1;
    server = std::make_unique<Server>(*server_link, c);
  }

  Client& add_client(int i, ClientConfig cc = {}) {
    Peer p;
    p.link = net.make_link(client_addr(i));
    if (cc.seed == 0) cc.seed = static_cast<std::uint64_t>(i) * 104729 + 13;
    p.client = std::make_unique<Client>(*p.link, kServer, cc);
    clients.push_back(std::move(p));
    return *clients.back().client;
  }

  void step(Micros tick = 1000) {
    for (auto& c : clients) c.client->update(net.now());
    server->update(net.now());
    while (auto e = server->poll_event()) events.push_back(*e);
    net.advance(tick);
  }
  void run(Micros duration, Micros tick = 1000) {
    for (Micros t = 0; t < duration; t += tick) step(tick);
  }

  int count(ServerEvent::Type t) const {
    int n = 0;
    for (auto& e : events) n += e.type == t;
    return n;
  }

  SimNetwork net;
  std::unique_ptr<SimLink> server_link;
  std::unique_ptr<Server> server;
  std::vector<Peer> clients;
  std::vector<ServerEvent> events;
};

void connects_on_a_clean_link() {
  World w(1);
  w.net.set_default_conditions({.delay = 10'000});
  Client& c = w.add_client(0);
  c.connect(w.net.now());
  w.run(200'000);
  CHECK(c.state() == Client::State::Connected);
  CHECK(c.connection_id() != 0);
  CHECK(w.server->connection_count() == 1);
  CHECK(w.count(ServerEvent::Type::Connected) == 1);
  CHECK(w.events[0].id == c.connection_id() && w.events[0].address == client_addr(0));
  CHECK(c.handshake_rtt().has_value());
  CHECK(*c.handshake_rtt() >= 20'000 && *c.handshake_rtt() <= 23'000);
  CHECK(w.server->stats().amplification_blocked == 0);
}

void connects_through_loss_jitter_reorder_and_duplicates() {
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    World w(seed);
    LinkConditions c;
    c.drop = 0.3;
    c.delay = 20'000;
    c.jitter = 20'000;
    c.reorder = 0.1;
    c.reorder_delay = 60'000;
    c.duplicate = 0.2;
    w.net.set_default_conditions(c);
    Client& a = w.add_client(0);
    Client& b = w.add_client(1);
    a.connect(0);
    b.connect(0);
    w.run(5'000'000);
    CHECK(a.state() == Client::State::Connected && b.state() == Client::State::Connected);
    CHECK(a.connection_id() != b.connection_id());
    CHECK(w.server->connection_count() == 2);
    CHECK(w.count(ServerEvent::Type::Connected) == 2);  // duplicates make no extra connections
    CHECK(w.count(ServerEvent::Type::Disconnected) == 0);
  }
}

void a_lost_accept_is_repeated() {
  World w(3);
  w.net.set_default_conditions({.delay = 10'000});
  Client& c = w.add_client(0);
  c.connect(0);
  // Let the Challenge out, then cut the server off: the Accept is sent into the void.
  while (w.server->stats().challenges_sent == 0) w.step();
  w.net.set_conditions(kServer, client_addr(0), {.drop = 1, .delay = 10'000});
  w.run(1'000'000);
  CHECK(c.state() == Client::State::Connecting);
  CHECK(w.server->connection_count() == 1 && w.server->stats().accepts_sent >= 1);
  w.net.set_conditions(kServer, client_addr(0), {.delay = 10'000});
  w.run(1'000'000);
  CHECK(c.state() == Client::State::Connected);
  CHECK(w.server->connection_count() == 1 && w.count(ServerEvent::Type::Connected) == 1);
  CHECK(w.server->stats().accepts_sent >= 2);  // answered again, no second connection
}

void absent_server_fails_after_the_timeout() {
  SimNetwork net(4);
  auto link = net.make_link(client_addr(0));
  Client c(*link, kServer, {.seed = 5, .handshake_timeout = 1'000'000});
  c.connect(0);
  for (int i = 0; i < 1100; ++i) {
    c.update(net.now());
    net.advance(1000);
  }
  CHECK(c.state() == Client::State::Failed);
  CHECK(link->stats().sent_datagrams >= 3 && link->stats().sent_datagrams <= 6);  // retried, not flooded
}

// Never larger than the request, and nothing at all for a request that is too small.
void replies_are_never_larger_than_requests() {
  ServerConfig cfg;
  cfg.seed = 9;
  cfg.init_rate_per_address = 1e6;
  cfg.init_burst_per_address = 1e6;
  cfg.init_rate_total = 1e6;
  cfg.init_burst_total = 1e6;
  World w(5, cfg);
  w.net.set_max_payload(kMaxDatagram);
  std::mt19937_64 rng(42);
  const Address victim = Address::v4(203, 0, 113, 7, 9999);
  auto vlink = w.net.make_link(victim);

  std::uint64_t requests = 0, replies = 0;
  auto probe = [&](std::span<const std::uint8_t> request) {
    const std::uint64_t before = vlink->stats().received_bytes;
    w.net.inject(victim, kServer, request);
    w.net.advance(0);
    w.server->update(w.net.now());
    w.net.advance(0);
    const std::uint64_t got = vlink->stats().received_bytes - before;
    CHECK(got <= request.size());
    requests += request.size();
    replies += got;
    Address from;
    std::uint8_t buf[2048];
    while (vlink->receive(from, buf)) {
    }
  };

  for (std::size_t size = 0; size <= kMaxDatagram; ++size) {
    PacketBuilder init(Header{PacketType::Init});
    init.init(rng());
    init.pad_to(size);
    probe(init.bytes());  // sizes below the minimum are not even answered...
    if (size < kMinHandshakeRequest) CHECK(vlink->stats().received_datagrams == 0);
    PacketBuilder resp(Header{PacketType::Response});
    resp.response({rng(), 0, rng()});
    resp.pad_to(size);
    probe(resp.bytes());
  }
  CHECK(vlink->stats().received_datagrams > 0);  // ...while full-size ones are
  CHECK(replies < requests);

  // Random datagrams, a third of them with a valid prefix.
  for (int i = 0; i < 20000; ++i) {
    std::vector<std::uint8_t> d(rng() % (kMaxDatagram + 1));
    for (auto& x : d) x = static_cast<std::uint8_t>(rng());
    if (d.size() >= 4 && i % 3 == 0) {
      d[0] = kMagic >> 8;
      d[1] = kMagic & 0xff;
      d[2] = kVersion;
      d[3] = static_cast<std::uint8_t>(1 + rng() % 5);
    }
    probe(d);
  }
  CHECK(w.server->stats().amplification_blocked == 0);
  CHECK(w.server->connection_count() == 0);
}

// Hand-driven client: gets a real Challenge so the test can forge Responses from it.
struct Prober {
  Prober(World& w, Address a) : w(w), addr(a), link(w.net.make_link(a)) {}

  ChallengeBody challenge(std::uint64_t nonce) {
    PacketBuilder init(Header{PacketType::Init});
    init.init(nonce);
    init.pad_to(kMinHandshakeRequest);
    link->send(kServer, init.bytes());
    w.run(5000);
    std::uint8_t buf[2048];
    Address from;
    const auto n = link->receive(from, buf);
    CHECK(n.has_value());
    Packet p;
    CHECK(parse_packet({buf, *n}, p) == ParseError::None && p.header.type == PacketType::Challenge);
    ChallengeBody c;
    CHECK(parse_challenge(p.body, c) == ParseError::None);
    CHECK(c.client_nonce == nonce);
    return c;
  }

  void respond(SimLink& from, const ChallengeBody& c) {
    PacketBuilder r(Header{PacketType::Response});
    r.response(c);
    r.pad_to(kMinHandshakeRequest);
    from.send(kServer, r.bytes());
    w.run(5000);
  }

  World& w;
  Address addr;
  std::unique_ptr<SimLink> link;
};

void cookies_must_be_right_for_this_address_nonce_and_time() {
  World w(6);
  Prober p(w, client_addr(0));
  Prober other(w, client_addr(1));
  const ChallengeBody c = p.challenge(0x1234);

  ChallengeBody bad = c;
  bad.cookie ^= 1;
  p.respond(*p.link, bad);
  bad = c;
  bad.client_nonce ^= 1;  // a different nonce than the cookie covers
  p.respond(*p.link, bad);
  bad = c;
  bad.bucket += 1;
  p.respond(*p.link, bad);
  p.respond(*other.link, c);  // a cookie replayed from another address
  CHECK(w.server->connection_count() == 0);
  CHECK(w.server->stats().bad_cookie == 4);
  CHECK(w.server->stats().accepts_sent == 0);

  // After the cookie ages out (two buckets) it is dead even for its owner.
  w.net.advance(25'000'000);
  p.respond(*p.link, c);
  CHECK(w.server->connection_count() == 0 && w.server->stats().bad_cookie == 5);

  const ChallengeBody fresh = p.challenge(0x1234);
  p.respond(*p.link, fresh);
  CHECK(w.server->connection_count() == 1);
  p.respond(*p.link, fresh);  // repeated: same connection, another Accept
  CHECK(w.server->connection_count() == 1 && w.server->stats().accepts_sent == 2);
  CHECK(w.count(ServerEvent::Type::Connected) == 1);
}

void init_floods_are_limited_and_cost_no_state() {
  World w(7);
  const Address flooder = Address::v4(198, 51, 100, 1, 4000);
  auto fl = w.net.make_link(flooder);
  PacketBuilder init(Header{PacketType::Init});
  init.init(1);
  init.pad_to(kMinHandshakeRequest);
  for (int i = 0; i < 1000; ++i) fl->send(kServer, init.bytes());
  w.run(10'000);
  CHECK(w.server->stats().challenges_sent <= 10);  // the per-address burst
  CHECK(w.server->stats().rate_limited >= 990);
  // The same IP from other ports shares the limit.
  auto fl2 = w.net.make_link(Address::v4(198, 51, 100, 1, 4001));
  for (int i = 0; i < 100; ++i) fl2->send(kServer, init.bytes());
  w.run(10'000);
  CHECK(w.server->stats().challenges_sent <= 10 + 1);
  // Others are unaffected, and the flooder is served again as tokens refill.
  w.add_client(0).connect(w.net.now());
  w.run(300'000);
  CHECK(w.clients[0].client->state() == Client::State::Connected);
  const auto sent = w.server->stats().challenges_sent;
  for (int i = 0; i < 20; ++i) fl->send(kServer, init.bytes());
  w.run(2'000'000);
  CHECK(w.server->stats().challenges_sent > sent);

  // Many spoofed sources: a bounded number of replies in all, and no state.
  World w2(8);
  PacketBuilder spoof(Header{PacketType::Init});
  spoof.init(2);
  spoof.pad_to(kMinHandshakeRequest);
  for (int i = 0; i < 5000; ++i) {
    w2.net.inject(Address::v4(100, static_cast<std::uint8_t>(i / 250), static_cast<std::uint8_t>(i % 250), 1, 1000), kServer,
                  spoof.bytes());
  }
  w2.run(10'000);
  CHECK(w2.server->stats().challenges_sent == 2000);  // init_burst_total
  CHECK(w2.server->connection_count() == 0);
}

void a_full_server_turns_clients_away_and_keeps_the_rest() {
  ServerConfig cfg;
  cfg.max_connections = 3;
  World w(9, cfg);
  for (int i = 0; i < 5; ++i) {
    w.add_client(i, {.handshake_timeout = 2'000'000}).connect(0);
  }
  w.run(2'500'000);
  int connected = 0, failed = 0;
  for (auto& p : w.clients) {
    connected += p.client->state() == Client::State::Connected;
    failed += p.client->state() == Client::State::Failed;
  }
  CHECK(connected == 3 && failed == 2);
  CHECK(w.server->connection_count() == 3 && w.server->stats().server_full > 0);
}

void a_restarted_client_replaces_its_old_connection() {
  World w(10);
  Client& a = w.add_client(0);
  a.connect(0);
  w.run(100'000);
  CHECK(a.state() == Client::State::Connected);
  const ConnectionId old_id = a.connection_id();
  // Same address, new process: a new Client on the same link with another nonce.
  Client again(*w.clients[0].link, kServer, {.seed = 777});
  again.connect(w.net.now());
  for (int i = 0; i < 100; ++i) {
    again.update(w.net.now());
    w.server->update(w.net.now());
    while (auto e = w.server->poll_event()) w.events.push_back(*e);
    w.net.advance(1000);
  }
  CHECK(again.state() == Client::State::Connected && again.connection_id() != old_id);
  CHECK(w.server->connection_count() == 1);
  CHECK(w.count(ServerEvent::Type::Connected) == 2 && w.count(ServerEvent::Type::Disconnected) == 1);
  CHECK(w.events[1].type == ServerEvent::Type::Disconnected && w.events[1].reason == DisconnectReason::Replaced);
}

void data_must_come_from_the_right_address_and_idle_connections_time_out() {
  ServerConfig cfg;
  cfg.idle_timeout = 2'000'000;
  World w(11, cfg);
  Client& c = w.add_client(0);
  c.connect(0);
  w.run(100'000);
  CHECK(c.state() == Client::State::Connected);
  auto stranger = w.net.make_link(client_addr(1));
  auto keepalive = [&](SimLink& from) {
    PacketBuilder b(Header{PacketType::Data, c.connection_id()});
    b.add_ping(1, 0);
    from.send(kServer, b.bytes());
  };
  // Right id from the wrong address does not keep it alive.
  for (int i = 0; i < 4; ++i) {
    keepalive(*stranger);
    w.run(1'000'000);
  }
  CHECK(w.server->stats().unknown_connection >= 4);
  CHECK(w.server->connection_count() == 0 && w.events.back().reason == DisconnectReason::Timeout);

  // From the right address it does (a new connection, for a fresh start).
  World w2(12, cfg);
  Client& c2 = w2.add_client(0, {.idle_timeout = 60'000'000});
  c2.connect(0);
  w2.run(100'000);
  for (int i = 0; i < 6; ++i) {
    PacketBuilder b(Header{PacketType::Data, c2.connection_id()});
    b.add_ping(1, 0);
    w2.clients[0].link->send(kServer, b.bytes());
    w2.run(1'000'000);
  }
  CHECK(w2.server->connection_count() == 1);
}

void either_side_can_close() {
  World w(13);
  Client& a = w.add_client(0);
  Client& b = w.add_client(1);
  a.connect(0);
  b.connect(0);
  w.run(200'000);
  CHECK(a.state() == Client::State::Connected && b.state() == Client::State::Connected);
  a.disconnect();
  w.run(50'000);
  CHECK(a.state() == Client::State::Closed && w.server->connection_count() == 1);
  CHECK(w.events.back().type == ServerEvent::Type::Disconnected && w.events.back().reason == DisconnectReason::Closed);
  w.server->disconnect(b.connection_id(), DisconnectReason::ServerShutdown);
  w.run(50'000);
  CHECK(b.state() == Client::State::Closed && b.close_reason() == DisconnectReason::ServerShutdown);
  CHECK(w.server->connection_count() == 0);
  // A closed client can connect again.
  a.connect(w.net.now());
  w.run(200'000);
  CHECK(a.state() == Client::State::Connected && w.server->connection_count() == 1);
}

}  // namespace

int main() {
  connects_on_a_clean_link();
  connects_through_loss_jitter_reorder_and_duplicates();
  a_lost_accept_is_repeated();
  absent_server_fails_after_the_timeout();
  replies_are_never_larger_than_requests();
  cookies_must_be_right_for_this_address_nonce_and_time();
  init_floods_are_limited_and_cost_no_state();
  a_full_server_turns_clients_away_and_keeps_the_rest();
  a_restarted_client_replaces_its_old_connection();
  data_must_come_from_the_right_address_and_idle_connections_time_out();
  either_side_can_close();
  return 0;
}
