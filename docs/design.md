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

## Netcode model (decided: `docs/decisions/0001-netcode-model.md`)

The proposal for a deterministic engine such as fighter-physics: **input-based
rollback with an authoritative relay server**.

- Clients send inputs only.
- The server fixes the canonical input for each frame. A late input becomes
  the player's previous input, so a slow client can't stall the others.
- The server broadcasts the canonical inputs.
- Clients predict, then roll back and re-simulate on correction.
- Periodic state hashes catch desyncs; the server's state wins on a resync.

The alternative, server-authoritative snapshots with client prediction (FPS
style), costs more bandwidth and loses determinism's advantage, but tolerates a
non-deterministic simulation. The record explains why it was not chosen.

## Packet size

A datagram is at most 1200 bytes of UDP payload. With IPv6 (40) and UDP (8)
headers that is 1248 bytes on the wire, under the tailnet's 1280-byte MTU, so
no packet is fragmented by IP.

The wire format is documented at the top of `include/npk/transport/wire.hpp`:
a 16-byte header (magic, version, type, connection id, sequence, ack and ack
bits), handshake bodies, and for established connections a list of frames
(unreliable, reliable fragment, ping, pong, close). Handshake requests are
padded to at least 128 bytes so no reply can exceed the request.

The transport talks to a `Link` (send a datagram, receive one, both
non-blocking), never to a socket. `UdpSocket` is the POSIX implementation;
`SimNetwork`/`SimLink` is an in-process one with drop, delay, jitter, reorder
and duplicate, a virtual clock and a seed, so transport and session tests are
exact and repeatable. Time is always passed in (`Micros now`), never read.

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

How the transport does that (`server.hpp`):

- Init and Response are padded to 128 bytes or more; smaller ones get no
  reply. Challenge (36 bytes) and Accept (32) are smaller than any request
  that can cause them, and `send_reply` refuses a reply larger than its request.
- The server is stateless until a Response carries a valid cookie: SipHash-2-4
  of the source address, the client's nonce and a 10-second time bucket under a
  secret from the OS. A cookie is valid for its bucket and the next. Only then
  does it allocate a connection (at most `max_connections`) and issue a
  connection id derived with a keyed hash, so ids cannot be guessed.
- Init, the one packet that costs a reply, is limited per source IP and in
  total (token buckets in a fixed-size table, so memory does not grow with the
  number of sources). A Response is checked before any limit, so a valid cookie
  is never starved by a spoofer sending Inits from the client's address.
- Data packets must carry a known connection id from that connection's address,
  or they are dropped silently. There is no connection migration.
- Not covered yet: confidentiality and integrity of game packets. An on-path
  attacker can read, replay or inject; see the encryption note above.

## Dependencies

Prefer the standard library and the OS socket API
(`docs/decisions/0002-dependencies.md`). Any third-party library
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
