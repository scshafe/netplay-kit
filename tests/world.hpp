#pragma once

// A simulated network with one server and any number of clients, stepped together.
// Links are destroyed before the network (members are declared in that order).

#include <memory>
#include <vector>

#include "npk/transport/client.hpp"
#include "npk/transport/server.hpp"
#include "npk/transport/sim_link.hpp"

namespace {

using namespace npk::transport;

const Address kServer = Address::v4(10, 0, 0, 1, 7000);

Address client_addr(int i) { return Address::v4(10, 1, static_cast<std::uint8_t>(i / 250), static_cast<std::uint8_t>(i % 250 + 1), 5000); }

struct ClientSlot {
  std::unique_ptr<SimLink> link;
  std::unique_ptr<Client> client;
  std::vector<Message> messages;  // what the client has received
};

struct World {
  explicit World(std::uint64_t seed, const ServerConfig& cfg = {}) : net(seed) {
    server_link = net.make_link(kServer);
    ServerConfig c = cfg;
    if (c.seed == 0) c.seed = seed * 7919 + 1;
    server = std::make_unique<Server>(*server_link, c);
  }

  Client& add_client(int i, ClientConfig cc = {}) {
    ClientSlot p;
    p.link = net.make_link(client_addr(i));
    if (cc.seed == 0) cc.seed = static_cast<std::uint64_t>(i) * 104729 + 13;
    p.client = std::make_unique<Client>(*p.link, kServer, cc);
    clients.push_back(std::move(p));
    return *clients.back().client;
  }

  void step(Micros tick = 1000) {
    if (!clients_silent) {
      for (auto& c : clients) c.client->update(net.now());
    }
    server->update(net.now());
    while (auto e = server->poll_event()) events.push_back(*e);
    while (auto m = server->poll_message()) server_messages.push_back(std::move(*m));
    for (auto& c : clients) {
      while (auto m = c.client->poll_message()) c.messages.push_back(std::move(*m));
    }
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

  bool clients_silent = false;  // the clients are not run, so they send nothing
  SimNetwork net;
  std::unique_ptr<SimLink> server_link;
  std::unique_ptr<Server> server;
  std::vector<ClientSlot> clients;
  std::vector<ServerEvent> events;
  std::vector<ServerMessage> server_messages;
};

}  // namespace
