#pragma once

// A simulated match: one ServerSession and some ClientSessions, each with its
// own ToyGame, over a SimNetwork, stepped together on a virtual clock. The
// server records the canonical inputs, so a test can replay them on a fresh game
// and know the right hash at every frame.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "npk/session/client_session.hpp"
#include "npk/session/server_session.hpp"
#include "npk/transport/sim_link.hpp"
#include "toy_game.hpp"

namespace {

using namespace npk;
using npk::session::Frame;
using npk::transport::Address;
using npk::transport::Micros;

struct Match {
  struct Player {
    std::unique_ptr<transport::SimLink> link;
    std::unique_ptr<ToyGame> game;
    std::unique_ptr<session::ClientSession> session;
    Micros skew = 0;                   // this client's clock runs this far ahead
    bool silent = false;               // not updated: sends nothing
  };

  explicit Match(std::uint64_t seed, std::size_t players = 2, session::ServerSessionConfig sc = {},
                 session::ClientSessionConfig cc = {}, std::size_t sessions = SIZE_MAX)
      : net(seed), players_n(players), server_game(players) {
    server_addr = Address::v4(10, 0, 0, 1, 7000);
    server_link = net.make_link(server_addr);
    if (sc.transport.seed == 0) sc.transport.seed = seed * 31 + 7;
    sc.on_canonical = [this](Frame, std::span<const std::uint8_t> in) {
      canonical.insert(canonical.end(), in.begin(), in.end());
    };
    server = std::make_unique<session::ServerSession>(*server_link, server_game, sc);
    for (std::size_t i = 0; i < std::min(players, sessions); ++i) {
      Player p;
      const auto me = Address::v4(10, 1, 0, static_cast<std::uint8_t>(i + 1), 5000);
      p.link = net.make_link(me);
      p.game = std::make_unique<ToyGame>(players);
      // This player's input per wall frame, 2 bytes each.
      auto script = std::make_shared<std::vector<std::uint8_t>>(toy_inputs(seed * 100 + i, 1, 20000));
      session::ClientSessionConfig c = cc;
      if (c.transport.seed == 0) c.transport.seed = seed * 977 + i + 1;
      p.session = std::make_unique<session::ClientSession>(
          *p.link, server_addr, *p.game,
          [script](Frame wall, std::span<std::uint8_t> out) {
            const std::size_t at = (wall % 20000) * 2;
            out[0] = (*script)[at];
            out[1] = (*script)[at + 1];
          },
          c);
      players_.push_back(std::move(p));
    }
  }

  void connect_all() {
    for (auto& p : players_) p.session->connect(net.now() + p.skew);
  }

  void step(Micros tick = 1000) {
    for (auto& p : players_) {
      if (!p.silent) p.session->update(net.now() + p.skew);
    }
    server->update(net.now());
    net.advance(tick);
  }
  void run(Micros duration, Micros tick = 1000) {
    for (Micros t = 0; t < duration; t += tick) step(tick);
  }

  // The hash of the right state at every frame the server finalised, 0..frames.
  std::vector<std::uint64_t> reference_hashes() const {
    std::vector<std::uint64_t> h;
    ToyGame g(players_n);
    const std::size_t fb = players_n * 2;
    for (std::size_t f = 0; f * fb < canonical.size(); ++f) {
      h.push_back(g.hash());
      g.step(static_cast<Frame>(f), std::span(canonical).subspan(f * fb, fb));
    }
    h.push_back(g.hash());
    return h;
  }

  transport::SimNetwork net;
  std::size_t players_n;
  Address server_addr;
  ToyGame server_game;
  std::unique_ptr<transport::SimLink> server_link;
  std::unique_ptr<session::ServerSession> server;
  std::vector<Player> players_;
  std::vector<std::uint8_t> canonical;
};

}  // namespace
