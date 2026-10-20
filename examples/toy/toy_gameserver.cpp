// The example game server: the toy game behind the kit's game server. A real
// game replaces the factory and links the same library.

#include "npk/gameserver/main.hpp"
#include "toy_game.hpp"

int main() {
  return npk::gameserver::run([](std::size_t players, const npk::json::Value& options) -> std::unique_ptr<npk::session::Simulation> {
    if (players < 1 || players > ToyGame::kMaxPlayers) return nullptr;
    auto g = std::make_unique<ToyGame>(players);
    // "end_after": the game ends after this many frames (so a demo match ends by itself).
    if (const auto* e = options.find("end_after"); e && e->as_int() && *e->as_int() > 0 && *e->as_int() < 1'000'000)
      g->end_after = static_cast<npk::session::Frame>(*e->as_int());
    return g;
  });
}
