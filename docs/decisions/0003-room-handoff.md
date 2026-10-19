# 0003: Handing a room to a game server

Status: accepted, 2026-10-19 (decided with the owner session, from the deploy model).

## Context

The lobby makes rooms (seats, ready, start); a game server runs matches. Something
has to give a ready room to a game server, and tell the players where to connect
and how to prove they belong.

The services deploy through the scshafe runner lane: each is a container the lane
starts and keeps running. The lane grants no Docker socket, so a container cannot
start other containers.

## Decision

- The game server is a **long-running container that hosts many matches**. The
  lobby does not start processes.
- A game server **registers with the lobby** and keeps registering (a heartbeat
  every few seconds): its control URL, its public UDP address, its capacity
  (`max_matches`) and how many matches it holds. The lobby forgets one that
  stops. Nothing about game servers is configured in the lobby.
- When a room is ready, the lobby picks a registered server with room and
  **assigns the room over a control channel**: a small HTTP API on the game
  server (`POST /rooms`) carrying the room id, the seats and, for each seat, a
  **session token** the lobby generated (128 random bits). The game server
  answers 201, or 409 when it is full, so the lobby tries another or refuses.
- Players learn the server's UDP address, the room id and their own token from
  the lobby (only their own). The client's first message on the game connection
  is a reliable `Join` (room id, token). The game server seats the connection
  only if the token matches a free seat of that room (compared in constant time);
  a connection that does not join within a few seconds, or joins wrongly three
  times, is closed.
- The game server **reports the result to the lobby** the same way: `POST` to the
  lobby's control API when a match ends (the room id, how it ended, the game's
  own result from `Simulation`).
- **Capacity** is a configured `max_matches` per server. A full server answers 409;
  the lobby refuses to start a room when no registered server has room.
- **Authentication of the control channel** is a shared secret. It is supplied to
  each container by name through the stack's `.env` (the container reads the
  variable named by `NPK_CONTROL_SECRET_VAR`, default `NPK_CONTROL_SECRET`), sent
  as `Authorization: Bearer <secret>`, compared in constant time, never logged,
  and never put in a URL. The control API is meant to be reachable only from the
  lobby (the stack's network), and a missing or empty secret makes the server
  refuse to start.

## Alternatives considered

- **The lobby launches a game-server process or container per room.** Needs the
  Docker socket or root-equivalent rights, which the lane never grants.
- **Game servers configured in the lobby (a static list).** Works, but each
  change of the stack needs a lobby change; registration needs none.
- **Tokens as MACs the game server verifies with a key, no assignment call.**
  Avoids sending tokens, but then a game server cannot refuse a room when full
  and the lobby cannot know capacity; the assignment is wanted anyway, and random
  tokens need no cryptography beyond a good source of randomness.

## Consequences

- Plain HTTP carries the secret and the tokens. On the tailnet or a private
  stack network that is accepted; before any public hosting the control and lobby
  APIs must sit behind TLS (a reverse proxy), which needs no change to the
  protocol. Game packets are a separate matter (design, Security).
- A game server restart loses its matches. The lobby sees the registration
  lapse and marks those rooms failed. Reconnecting into a running match is not
  part of this decision.
- The `Simulation` interface gains an optional end-of-match report
  (`finished()`, `result_json()`), so the server can decide when a match is
  over and what to tell the lobby without knowing the game.
