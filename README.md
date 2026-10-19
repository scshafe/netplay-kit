# netplay-kit

A reusable C++ framework for online multiplayer games. It has a UDP
transport, rollback netcode for deterministic simulations, an authoritative
game server, a lobby and a home page, and its services ship as Docker images.
Its first user is [ledgefall](https://github.com/scshafe/ledgefall).

- Brief: [docs/brief.md](docs/brief.md). Design: [docs/design.md](docs/design.md).
- C++20, CMake.

## Build and test

```sh
./scripts/verify      # configure, build and run the tests (CMake + CTest)
```

## License

MIT (`LICENSE`).
