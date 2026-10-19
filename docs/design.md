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

`Connection` (`connection.hpp`) is one established connection: packet sequence
numbers with an ack and a 32-packet ack bitfield in every header, an unreliable
channel (at most once, bounded queue, oldest dropped) and a reliable ordered
channel (messages up to 64 KiB by default, cut into fragments, a 128-fragment
window, resend on a round-trip-derived timeout with backoff or when three later
packets were acked, delivery once and in order). A packet is validated whole
before it is acked or applied, so an ack means every frame in it was taken, and
a duplicate packet is dropped whole. Every buffer has a bound and a peer that
breaks one is disconnected with `ProtocolError`. `Server` and `Client` own the
connections: `send_unreliable`, `send_reliable`, `poll_message`.

Round trip and clock offset (`clock_sync.hpp`): each end pings the other (eight
pings 50 ms apart after connecting, then one a second). A pong carries the
pinger's send time and the responder's receive and send times, so the
responder's holding time is removed from the round trip, and the clock offset
(peer clock minus local) comes out of the four timestamps. The offset is taken
from the lowest-round-trip sample of the last 16, which bounds its error by half
that sample's queueing excess; it cannot see an unequal split between the two
directions (10 ms up, 40 ms down reads as 15 ms of offset). The handshake
supplies a first sample. Pings are answered at most four per flush, so a packet
of pings cannot draw out a bigger reply, and a pong is only accepted for a ping
that was sent and not yet answered, with its own timestamp echoed. The session
layer reads `clock()` (smoothed `rtt()`, `min_rtt()`, `jitter()`, `offset()`);
the retransmission timer keeps its own estimate from acks.

## Session layer (`npk::session`)

A game plugs in by implementing `Simulation` (`simulation.hpp`): `step(frame,
inputs)`, `snapshot`, `restore`, `hash`, a fixed `input_size()`, and optionally
`valid_input`, `neutral_input` and `predict_input`. Inputs are opaque bytes of
the game's own encoding, which keeps every length in the protocol bounded and
lets the server check them without knowing the game. The simulation must be
deterministic (the header says what that rules out). "The state at frame f" is
the state before `step(f, ...)`.

`Rollback` (`rollback.hpp`) is the client-side core and knows nothing of the
network. It simulates ahead, using the local player's *provisional* input and,
for everyone else, a prediction (the previous input again by default). A
*canonical* input arrives from the server for each player and frame; when one
differs from what was simulated, the state is restored from the snapshot at
that frame and the frames since are simulated again. A frame is *confirmed*
once simulated with canonical inputs only. Bounds: at most `max_rollback` (16)
frames past the confirmed ones, which is also the deepest possible rollback and
the number of snapshots held; inputs are accepted only for the `window` (64)
frames from the confirmed one; wrong sizes, invalid inputs and out-of-window
frames are refused and counted; a repeated canonical frame is ignored (the
first wins). Every `hash_interval` (30) frames, when the state at that frame
becomes confirmed, a *checkpoint* (frame, hash) is queued for the session to
report. `resync(frame, snapshot)` adopts the server's state at a frame.

**Who does what.** The server embeds the game's simulation through the same
interface (decided; see the Parts table). It collects each player's input for a
frame and finalises the canonical inputs of frame F when all have arrived or
when its clock passes the frame's start plus a grace period; a missing input
becomes the player's previous canonical one and a late one is dropped. It steps
its own simulation with the canonical inputs, keeps its hash at every
checkpoint, and compares the hashes clients report. A client that disagrees at
a checkpoint is sent the server's newest checkpoint state (a reliable message),
which it adopts with `resync`; the server's state wins. A client asks for
canonical frames from `next_canonical_needed()` on, so what a resync dropped is
sent again.

**Sessions over the transport** (`server_session.hpp`, `client_session.hpp`,
messages in `messages.hpp`). `ServerSession` seats the first `player_count`
clients in order of arrival and, when all are seated, sends each a reliable
`Start` (seat, tick rate, and the server-clock time at which frame 0 begins).
Frame F begins at start + F / tick rate. Each client turns its measured clock
offset into the *wall frame* (the frame that "now" is), and once per wall frame
samples its input, labels it for frame wall + delay, uses it at once as the
provisional input, and sends the last (up to 32) unconfirmed labelled inputs in
one unreliable `Inputs` message together with the first canonical frame it
still needs. Loss costs nothing because every message repeats what is
unconfirmed. The server finalises frame F when every seated player's input is
in, or at the frame's start plus `grace_frames` (2), and never more than
`max_lead_frames` (8) ahead of the clock; it then steps its own simulation, and
sends each client the canonical frames from that client's acknowledged one
(`Frames`, unreliable, redundant, at most 1100 bytes of inputs a message).
A missing input repeats the player's last canonical one, a player who left
plays neutral input, and an input for a finalised frame is dropped. The first
input received for a frame wins. A client that sends too many malformed or
invalid messages is dropped; a game with more than 16 players, inputs over 255
bytes or more than 1100 bytes of input per frame is refused. A client that
arrives when the seats are full is closed.

**Desync detection and repair.** Every `hash_interval` (30) frames, when the
state at that frame becomes confirmed, a client sends the server a reliable
`Hash` (frame, hash); `Start` carries the interval and a client with another
one refuses to play, so a misconfiguration is loud. The server keeps its own
hash and snapshot at the last eight checkpoints and compares. On a mismatch it
sends that client a reliable `Resync` with its newest checkpoint state, which
the client adopts (`Rollback::resync`) and plays on from, asking again for the
canonical frames it needs. Hashes for frames older than the one sent are stale
and ignored. The server's state wins always; a client that needs more than
`max_resyncs` (5) repairs is dropped, since it cannot stay in step. The same
state transfer catches up a client that fell further behind than the server
keeps canonical frames (256, about four seconds): it does not count against the
client. The client checks what it is sent: a `Resync` must come on the reliable
channel, fit the message bound, be restorable, and name a frame near the
clock; a `Start` must match its game; a canonical input the game rejects ends
the session. A snapshot must fit one reliable message (64 KiB by default,
`ConnectionConfig::max_message_size`), or the state cannot be repaired and the
client is dropped.

**Input delay** (`input_delay.hpp`; decided with the owner session). Frames of
delay = ceil((smoothed RTT / 2 + k * jitter) / frame time), with k = 2, jitter
the mean deviation of the round trip, clamped to 1..6 frames at 60 Hz. It is
set when the match starts and re-evaluated about twice a second at a safe
point: a frame boundary outside any resimulation (never in the middle of a
rollback). It changes only with hysteresis: a raise needs three evaluations in
a row that want one, then goes to the target; a drop needs three and also 60
frames without a rollback, and goes down one frame at a time. The delay is a
client-side labelling choice (an input sampled at wall frame W is labelled
W + delay), so the server and the canonical stream do not depend on it. Rollback
covers what the delay does not.

## Lobby and game server (milestone 3)

Decided in `docs/decisions/0003-room-handoff.md` and `0004-http-and-json.md`.

```
 player ---HTTP/JSON---> npk-lobby <---register, results, assign (HTTP/JSON, shared secret)---> npk-gameserver
   |                                                                                               ^
   +------------------------ UDP: Join(room, token), then the match ------------------------------+
```

- **Game server**: one long-running container, many matches, `max_matches`
  from its config. It owns one UDP port for all of them and one control HTTP
  port. It registers with the lobby every few seconds, accepts `POST /rooms`
  (room id, seats with their tokens) from the lobby only, refuses with 409 when
  full, and reports each result to the lobby.
- **Lobby**: rooms, seats, ready and start, as HTTP + JSON; long-polling for
  changes. A room that is full and ready is assigned to a registered game server
  that has room. Each player gets the server's UDP address, the room id and their
  own token, and no one else's.
- **Control authentication**: `Authorization: Bearer <shared secret>`, compared in
  constant time, from a variable named in the environment. Plain HTTP: before public
  hosting, a reverse proxy adds TLS.
- **The game never appears here.** The game server is a library plus a `main`
  helper that a game links with its `Simulation`; netplay-kit ships a toy-game
  server as the example and for the compose file. `Simulation` gains an optional
  `finished()` and `result_json()`.

Planned PRs: in-tree JSON; HTTP server and client; matches hosted in rooms
with tokens, and the game server; the lobby; the executables, images and a
compose file with an end-to-end test.

## Network path (measured)

The scshafe runner lane puts each service on its own tailnet node: a Tailscale
sidecar in userspace mode, with the service in the sidecar's network namespace.
`tailscale serve` proxies TCP and HTTPS only, and game traffic is UDP, so the
question was whether UDP from a tailnet peer reaches the game server through such
a node, and what it costs.

**Result (issue #8, run by the owner session on 2026-10-19 with `npk_echo`, 1000
datagrams of 64, 512 and 1200 bytes every 16 ms, two runs each; Tailscale 1.102 to
1.104, sidecar image 1.102.5):** the sidecar model works for game UDP. Echo in the
network namespace of a userspace sidecar, with no published port and no `serve`,
was reached on the node's tailnet address on UDP 7777.

| Path | p50 at 1200 B | loss at 1200 B | reordering | jitter |
| --- | --- | --- | --- | --- |
| LAN (host port, Docker publishing) | 2.4 ms | 0% | 0 | 0.7 ms |
| PC's tailnet address (host `tailscaled`, direct) | 3.3-3.6 ms | 0% | 0 | 0.8-1.1 ms |
| Runner-lane shape (userspace sidecar, direct) | 3.5-3.6 ms | 0-0.1% | 0 | 0.9-1.1 ms |

The tailnet adds about 1 ms at the median over the LAN, the userspace sidecar adds
nothing measurable over the host's `tailscaled`, and 1200-byte datagrams (1248 on
the wire with IPv6 and UDP headers) pass, which confirms the packet size choice.
The spread (p95 - p50) is 1.6-2.6 ms. The occasional lost packet (at most 0.3%,
at 64 and 512 bytes as well as 1200) is not an MTU effect.

Caveats to carry forward:

- A new node spends its first few seconds relayed through DERP (about 90 ms round
  trip) until the direct path is set up. A game server's node is long-lived, so
  this is the players' first seconds only if their own paths are not direct yet;
  the session layer's clock sync and input delay adapt to whatever the path is.
- The test peers shared a LAN, so the direct path was easy. A player elsewhere
  depends on NAT traversal to the host, or else on DERP at tens of milliseconds
  more; the lobby may want to show the measured round trip before a match.
- The tailnet's policy is still allow-all, so UDP was allowed without a change.
  When that grant is narrowed, the game server's node needs its own grant, for
  example `{src: [<players>], dst: ["tag:<game>"], ip: ["udp:7777"]}` (the tag
  and the port are the game's; the infra repository owns the policy).

Options that were kept in reserve and are not needed now: a kernel-mode sidecar,
a native node (tsnet), a host port on the PC's tailnet address.

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
   identical hashes. In three steps: the rollback core and the delay rule (done);
   the session messages and the server and client sessions over the transport;
   hash checks, resync and hostile-input tests.
3. The game server and lobby executables, plus Docker images (see "Lobby and
   game server").
4. **The UDP-over-tailnet proof** on the PC (with the owner session): done, see
   "Network path (measured)".
5. The home page.
