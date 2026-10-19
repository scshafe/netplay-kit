# netplay-kit: design (starting point)

The brief (`docs/brief.md`) is fixed; this document is the designer's, and the
project's agent keeps it in step with the code. Hard-to-reverse choices get a
record in `docs/decisions/`.

## Parts

| Part | Where it runs | What it does |
| --- | --- | --- |
| `npk::transport` | client and server | UDP sockets, connection handshake, sequence numbers and acks, an unreliable channel (inputs, state) and a reliable ordered channel (control), MTU-safe packets, round-trip and clock-offset estimation |
| `npk::session` | client and server | The netcode, generic over a game's `Simulation` interface (`step(frame, inputs)`, `snapshot`, `restore`, `hash`, input encode and decode): input exchange, prediction, rollback and re-simulation, input delay, desync detection by hash |
| `npk-gameserver` | container | Hosts matches. It is authoritative: it orders and timestamps inputs, relays them, checks hashes and decides the result. It embeds a game's simulation through the same interface |
| `npk-lobby` | container | HTTP + JSON (and a push channel, such as WebSocket or long-poll): rooms, seats, ready and start, and handing a room to a game server |
| Home page | served by the lobby | A small static site: the game, a download link for the macOS app, how to join |

## Netcode model (decide first; record it)

The proposal for a deterministic engine such as fighter-physics: **input-based
rollback with an authoritative relay server**.

- Clients send inputs only.
- The server fixes the canonical input for each frame. A late input becomes
  the player's previous input, so a slow client can't stall the others.
- The server broadcasts the canonical inputs.
- Clients predict, then roll back and re-simulate on correction.
- Periodic state hashes catch desyncs; the server's state wins on a resync.

Record the alternative considered: server-authoritative snapshots with client
prediction, like FPS netcode. It costs more bandwidth and loses determinism's
advantage, but tolerates a non-deterministic simulation.

## Network path (prove early)

The scshafe runner lane puts each service on its own tailnet node. The node is
a Tailscale sidecar in userspace mode, and `tailscale serve` proxies TCP and
HTTPS only. Game traffic is UDP. The first milestone must prove that UDP from
a tailnet peer reaches the game server through the node. Measure the latency
and jitter it adds compared with LAN, and record the result. If the userspace
path is not good enough, the options are, recorded as a decision with the
owner:

- a kernel-mode sidecar (needs privileges the lane does not grant today);
- a native node, as lexinet does with tsnet (needs a C++-compatible Tailscale
  client);
- a host port on the PC's tailnet address.

The lobby is HTTP, so it fits the existing sidecar model as it is.

## Security

- The tailnet encrypts today, but don't depend on it.
- The design leaves room for authenticated encryption of game packets
  (for example a Noise-style handshake), plus session tokens issued by the
  lobby and checked by the game server.
- Rate-limit unauthenticated packets, and never amplify: a reply is never
  larger than the request before the handshake completes.

## Dependencies

Prefer the standard library and the OS socket API. Any third-party library
(an HTTP server, JSON, crypto) needs a decision record, a pinned version, and
a licence compatible with MIT.

## Deploy

Two images, `npk-lobby` and the game server. The game server image is built
by the game: ledgefall's server links netplay-kit. Both go through the scshafe
runner lane on the PC (`host = "pc"`), configured in the game's repository.
netplay-kit itself ships an example game and a compose file for local runs.

## First milestones

1. The transport: handshake, channels, acks, RTT. Loopback tests and a
   lossy-link simulator (drop, delay, jitter, reorder) in tests.
2. The `Simulation` interface and the rollback session, tested with a toy
   deterministic game and the lossy-link simulator. Clients must converge to
   identical hashes.
3. The game server and lobby executables, plus Docker images.
4. **The UDP-over-tailnet proof** on the PC (with the owner session).
5. The home page.
