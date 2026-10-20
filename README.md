# netplay-kit

A reusable C++ framework for online multiplayer games. It has a UDP
transport, rollback netcode for deterministic simulations, an authoritative
game server, a lobby and a home page, and its services ship as Docker images.
Its first user is [ledgefall](https://github.com/scshafe/ledgefall).

- Brief: [docs/brief.md](docs/brief.md). Design: [docs/design.md](docs/design.md).
- C++20, CMake.

## What is in it

| Part | Where |
| --- | --- |
| Transport: UDP, handshake, acks, reliable and unreliable channels, RTT and clock sync | `npk::transport` |
| Rollback netcode over your game's `Simulation` | `npk::session` |
| Game server (many matches, lobby-issued tokens, control API) | `npk::gameserver`, `npk::gameserver::run` |
| Lobby (rooms, seats, ready, long-poll) | `npk-lobby` (`apps/npk_lobby.cpp`) |
| An example game, its server and a bot | `examples/toy` |

## Try the example stack

```sh
cp .env.example .env            # then put a real secret in it: openssl rand -hex 24
docker compose up --build       # the lobby on :8080, the toy game server on udp :7777
build/toy_bot --lobby localhost:8080 --room demo --create 2 &   # after ./scripts/verify built it
build/toy_bot --lobby localhost:8080 --room demo
```

A game plugs in by implementing `npk::session::Simulation` and calling
`npk::gameserver::run(factory)` in its server's `main`; see `examples/toy` and
[docs/design.md](docs/design.md).

## Build and test

```sh
./scripts/verify      # configure, build and run the tests (CMake + CTest)
```

## License

MIT (`LICENSE`).
