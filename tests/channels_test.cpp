// Channels, acks and retransmission over the simulated network, and a connection
// under hostile packets.
#include <algorithm>
#include <random>
#include <vector>

#include "check.hpp"
#include "world.hpp"

using namespace npk::transport;

namespace {

using Bytes = std::vector<std::uint8_t>;

// A message whose content says what it is, so reordering or corruption shows.
Bytes make_message(std::uint32_t index, std::size_t size) {
  Bytes m(std::max<std::size_t>(size, 4));
  for (std::size_t i = 0; i < m.size(); ++i) m[i] = static_cast<std::uint8_t>((index * 131 + i * 7) & 0xff);
  m[0] = static_cast<std::uint8_t>(index);
  m[1] = static_cast<std::uint8_t>(index >> 8);
  m[2] = static_cast<std::uint8_t>(index >> 16);
  m[3] = static_cast<std::uint8_t>(index >> 24);
  m.resize(size < 4 ? 4 : size);
  return m;
}

std::uint32_t index_of(const Bytes& m) {
  return m[0] | (m[1] << 8) | (m[2] << 16) | (static_cast<std::uint32_t>(m[3]) << 24);
}

std::size_t size_for(std::uint32_t i) {
  static const std::size_t sizes[] = {4, 10, 300, 1023, 1024, 1025, 2048, 5000, 20000, 65536};
  return sizes[i % 10];
}

LinkConditions nasty() {
  LinkConditions c;
  c.drop = 0.2;
  c.delay = 25'000;
  c.jitter = 25'000;
  c.reorder = 0.1;
  c.reorder_delay = 80'000;
  c.duplicate = 0.15;
  return c;
}

void reliable_is_exactly_once_and_in_order_both_ways() {
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    World w(seed);
    w.net.set_default_conditions(nasty());
    Client& c = w.add_client(0);
    c.connect(0);
    w.run(3'000'000);
    CHECK(c.state() == Client::State::Connected);
    const ConnectionId id = c.connection_id();

    const std::uint32_t n = 120;
    std::vector<Bytes> up, down;
    for (std::uint32_t i = 0; i < n; ++i) {
      up.push_back(make_message(i, size_for(i)));
      down.push_back(make_message(i, size_for(i + 3)));
    }
    std::uint32_t sent_up = 0, sent_down = 0;
    for (Micros t = 0; t < 120'000'000 && (sent_up < n || sent_down < n || w.server_messages.size() < n || w.clients[0].messages.size() < n); t += 1000) {
      if (sent_up < n && c.send_reliable(up[sent_up])) ++sent_up;
      if (sent_down < n && w.server->send_reliable(id, down[sent_down])) ++sent_down;
      w.step();
    }
    CHECK(w.server_messages.size() == n && w.clients[0].messages.size() == n);
    for (std::uint32_t i = 0; i < n; ++i) {
      CHECK(w.server_messages[i].id == id && w.server_messages[i].message.channel == Channel::Reliable);
      CHECK(w.server_messages[i].message.data == up[i]);
      CHECK(w.clients[0].messages[i].channel == Channel::Reliable);
      CHECK(w.clients[0].messages[i].data == down[i]);
    }
    // Quiet again afterwards: nothing more arrives, nothing is still being resent.
    w.run(2'000'000);
    CHECK(w.server_messages.size() == n && w.clients[0].messages.size() == n);
    CHECK(w.server->connection_count() == 1 && c.state() == Client::State::Connected);
  }
}

void unreliable_arrives_at_most_once_and_intact() {
  World w(21);
  LinkConditions c = nasty();
  c.duplicate = 0.5;
  w.net.set_default_conditions(c);
  Client& cl = w.add_client(0);
  cl.connect(0);
  w.run(3'000'000);
  CHECK(cl.state() == Client::State::Connected);
  const std::uint32_t n = 3000;
  for (std::uint32_t i = 0; i < n; ++i) {
    CHECK(cl.send_unreliable(make_message(i, 8 + i % 200)));
    w.run(5'000, 1000);
  }
  w.run(1'000'000);
  std::vector<bool> got(n, false);
  for (auto& m : w.server_messages) {
    CHECK(m.message.channel == Channel::Unreliable);
    const std::uint32_t i = index_of(m.message.data);
    CHECK(i < n && !got[i]);  // never twice, although the network duplicates half of the packets
    CHECK(m.message.data == make_message(i, 8 + i % 200));
    got[i] = true;
  }
  CHECK(w.server_messages.size() > n * 6 / 10 && w.server_messages.size() < n);  // ~80% survive
  // The limits: too big, and the oldest dropped when the queue is full.
  CHECK(!cl.send_unreliable(Bytes(Connection::kMaxUnreliable + 1)));
  CHECK(cl.send_unreliable(Bytes(Connection::kMaxUnreliable)));
}

void a_dead_link_costs_few_resends_and_recovers() {
  World w(31);
  w.net.set_default_conditions({.delay = 20'000});
  Client& c = w.add_client(0);
  c.connect(0);
  w.run(500'000);
  const ConnectionId id = c.connection_id();
  (void)id;
  w.net.set_conditions(client_addr(0), kServer, {.drop = 1, .delay = 20'000});
  for (std::uint32_t i = 0; i < 5; ++i) CHECK(c.send_reliable(make_message(i, 3000)));
  const auto before = w.clients[0].link->stats().sent_datagrams;
  w.run(6'000'000);
  const auto during = w.clients[0].link->stats().sent_datagrams - before;
  CHECK(during < 80);  // backoff, not a flood: 15 fragments, a few doublings, keepalives
  CHECK(c.state() == Client::State::Connected);
  w.net.set_conditions(client_addr(0), kServer, {.delay = 20'000});
  w.run(8'000'000);
  CHECK(w.server_messages.size() == 5);
  for (std::uint32_t i = 0; i < 5; ++i) CHECK(w.server_messages[i].message.data == make_message(i, 3000));
}

void buffers_are_bounded_and_drain() {
  ServerConfig sc;
  sc.connection.max_send_buffer = 10'000;
  World w(41, sc);
  w.net.set_default_conditions({.delay = 10'000});
  ClientConfig cc;
  cc.connection.max_message_size = 8000;
  cc.connection.max_send_buffer = 20'000;
  Client& c = w.add_client(0, cc);
  c.connect(0);
  w.run(300'000);
  CHECK(!c.send_reliable(Bytes(8001)));           // over max_message_size
  CHECK(c.send_reliable(Bytes(8000, 1)));
  CHECK(c.send_reliable(Bytes(8000, 2)));
  CHECK(!c.send_reliable(Bytes(8000, 3)));        // 24000 > 20000 buffered
  CHECK(!w.server->send_reliable(9999, Bytes(1)));  // unknown connection
  w.run(2'000'000);
  CHECK(w.server_messages.size() == 2);
  CHECK(c.send_reliable(Bytes(8000, 4)));         // acked data frees the buffer
  CHECK(c.send_reliable({}));                     // an empty message is a message
  w.run(2'000'000);
  CHECK(w.server_messages.size() == 4 && w.server_messages[3].message.data.empty());
}

void rtt_settles_on_a_steady_link() {
  World w(51);
  w.net.set_default_conditions({.delay = 30'000});
  Client& c = w.add_client(0);
  c.connect(0);
  w.run(300'000);
  for (int i = 0; i < 200; ++i) {
    c.send_reliable(make_message(i, 100));
    w.server->send_unreliable(c.connection_id(), make_message(i, 20));
    w.run(20'000);
  }
  w.run(500'000);
  CHECK(w.server_messages.size() == 200);
  // 60 ms round trip, a 1 ms tick of ack delay on top: the estimate settles there,
  // and the retransmission timeout sits just above it, not at its initial 250 ms.
  const auto rtt = c.rtt();
  CHECK(rtt && rtt->has_sample());
  CHECK(rtt->srtt() >= 59'000 && rtt->srtt() <= 66'000);
  CHECK(rtt->rto() >= 60'000 && rtt->rto() <= 100'000);
  const auto srv = w.server->rtt(c.connection_id());
  CHECK(srv && srv->has_sample() && srv->srtt() <= 66'000);
  CHECK(w.clients[0].client->state() == Client::State::Connected);
}

// ---- hostile input ---------------------------------------------------------

struct Rig {
  explicit Rig(std::uint64_t seed) : w(seed) {
    c = &w.add_client(0);
    c->connect(0);
    w.run(200'000);
    CHECK(c->state() == Client::State::Connected);
    id = c->connection_id();
    w.clients_silent = true;  // from here the test speaks for the client
  }
  void send(const PacketBuilder& b) {
    w.clients[0].link->send(kServer, b.bytes());
    w.run(5'000);
  }
  World w;
  Client* c;
  ConnectionId id;
};

void protocol_violations_fail_the_connection() {
  {  // a fragment that continues a message that never began
    Rig r(61);
    PacketBuilder b(Header{PacketType::Data, r.id, 10});
    b.add_reliable(0, 0, Bytes(10));
    r.send(b);
    CHECK(r.w.server->connection_count() == 0);
    CHECK(r.w.events.back().reason == DisconnectReason::ProtocolError);
  }
  {  // a fragment far outside the window
    Rig r(62);
    PacketBuilder b(Header{PacketType::Data, r.id, 10});
    b.add_reliable(1000, kFragBegin | kFragEnd, Bytes(10));
    r.send(b);
    CHECK(r.w.server->connection_count() == 0);
  }
  {  // a begin inside a message
    Rig r(63);
    PacketBuilder b(Header{PacketType::Data, r.id, 10});
    b.add_reliable(0, kFragBegin, Bytes(10));
    b.add_reliable(1, kFragBegin, Bytes(10));
    r.send(b);
    CHECK(r.w.server->connection_count() == 0);
  }
  {  // a message that grows past the limit
    Rig r(64);
    for (std::uint16_t i = 0; i < 70 && r.w.server->connection_count() == 1; ++i) {
      PacketBuilder b(Header{PacketType::Data, r.id, static_cast<std::uint16_t>(10 + i)});
      b.add_reliable(i, i == 0 ? kFragBegin : 0, Bytes(kMaxFragment));
      r.send(b);
    }
    CHECK(r.w.server->connection_count() == 0);  // 64 KiB + 1 fragment
    CHECK(r.w.server_messages.empty());
  }
}

void a_malformed_packet_is_not_acked_and_not_applied() {
  Rig r(65);
  // Valid first frame, then a frame that runs past the end.
  PacketBuilder b(Header{PacketType::Data, r.id, 10});
  b.add_unreliable(Bytes{1, 2, 3});
  Bytes raw(b.bytes().begin(), b.bytes().end());
  raw.insert(raw.end(), {1, 0xff, 0xff, 7});
  r.w.clients[0].link->send(kServer, raw);
  r.w.run(5'000);
  CHECK(r.w.server_messages.empty());  // the valid frame was not applied either
  CHECK(r.w.server->connection_count() == 1);
  // The malformed packet's sequence number was not recorded, so a good packet with
  // the same number is still new.
  PacketBuilder good(Header{PacketType::Data, r.id, 10});
  good.add_unreliable(Bytes{1, 2, 3});
  r.send(good);
  CHECK(r.w.server_messages.size() == 1);  // seq 10 was still unseen, so it is taken
}

void acks_name_exactly_what_was_received() {
  Rig r(66);
  auto link = r.w.clients[0].link.get();
  auto send_seq = [&](std::uint16_t seq) {
    PacketBuilder b(Header{PacketType::Data, r.id, seq});
    b.add_unreliable(Bytes{static_cast<std::uint8_t>(seq)});
    link->send(kServer, b.bytes());
    r.w.run(5'000);
  };
  // Receive 100, 98, 97, 95, then 100 again, then one from far in the past.
  for (std::uint16_t s : {100, 98, 97, 95, 100, 40}) send_seq(s);
  CHECK(r.w.server_messages.size() == 4);  // the repeat and the stale one were dropped
  // The server's ack for those: latest 100, bits for 99(no) 98(yes) 97(yes) 96(no) 95(yes).
  r.w.run(2'000'000);  // its acks go out with the next packet it sends
  Address from;
  Bytes buf(2048);
  std::uint16_t ack = 0;
  std::uint32_t bits = 0;
  bool seen = false;
  while (auto n = link->receive(from, buf)) {
    Packet p;
    if (parse_packet({buf.data(), *n}, p) == ParseError::None && p.header.type == PacketType::Data) {
      ack = p.header.ack;
      bits = p.header.ack_bits;
      seen = true;
    }
  }
  CHECK(seen && ack == 100);
  CHECK(bits == 0b10110u);
}

void random_packets_to_a_live_connection_are_survivable() {
  Rig r(67);
  std::mt19937_64 rng(1234);
  auto* link = r.w.clients[0].link.get();
  for (int i = 0; i < 30000 && r.w.server->connection_count() == 1; ++i) {
    PacketBuilder b(Header{PacketType::Data, r.id, static_cast<std::uint16_t>(rng()), static_cast<std::uint16_t>(rng()),
                           static_cast<std::uint32_t>(rng())});
    const int frames = static_cast<int>(rng() % 5);
    for (int f = 0; f < frames; ++f) {
      Bytes payload(rng() % 200);
      for (auto& x : payload) x = static_cast<std::uint8_t>(rng());
      switch (rng() % 4) {
        case 0: b.add_unreliable(payload); break;
        case 1: b.add_reliable(static_cast<std::uint16_t>(rng() % 200), static_cast<std::uint8_t>(rng() % 4), payload); break;
        case 2: b.add_ping(static_cast<std::uint32_t>(rng()), rng()); break;
        default: b.add_pong(1, rng(), rng(), rng()); break;
      }
    }
    link->send(kServer, b.bytes());
    if (i % 16 == 0) r.w.run(1'000);  // let the server read, flush, and drop what it does not like
  }
  // Whatever the noise did, the server is still running (sanitizers watched), and a
  // connection that ended did so by the protocol's own verdict.
  r.w.run(10'000);
  CHECK(r.w.server->connection_count() == 1 || r.w.events.back().reason == DisconnectReason::ProtocolError);
}

}  // namespace

int main() {
  reliable_is_exactly_once_and_in_order_both_ways();
  unreliable_arrives_at_most_once_and_intact();
  a_dead_link_costs_few_resends_and_recovers();
  buffers_are_bounded_and_drain();
  rtt_settles_on_a_steady_link();
  protocol_violations_fail_the_connection();
  a_malformed_packet_is_not_acked_and_not_applied();
  acks_name_exactly_what_was_received();
  random_packets_to_a_live_connection_are_survivable();
  return 0;
}
