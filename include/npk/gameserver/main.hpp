#pragma once

// The whole of a game server's `main`, for a game to call with its Simulation
// factory:
//
//   int main() { return npk::gameserver::run(my_factory); }
//
// Configuration is from the environment (a container's natural interface):
//   NPK_CONTROL_SECRET_VAR  name of the variable holding the shared secret (default NPK_CONTROL_SECRET)
//   NPK_UDP_PORT            game traffic, default 7777
//   NPK_CONTROL_PORT        control API, default 9000
//   NPK_CONTROL_BIND        address for the control API, default 0.0.0.0
//   NPK_MAX_MATCHES         default 16
//   NPK_TICK_RATE           frames a second, default 60
//   NPK_LOBBY               host:port of the lobby to register with (optional)
//   NPK_ADVERTISE_CONTROL   host:port the lobby reaches the control API at (with NPK_LOBBY)
//   NPK_ADVERTISE_UDP       host:port players reach the game traffic at (with NPK_LOBBY)
// Returns a process exit code: 0 after SIGINT/SIGTERM, 2 for bad configuration.

#include "npk/gameserver/game_server.hpp"

namespace npk::gameserver {

int run(SimulationFactory factory);

}  // namespace npk::gameserver
