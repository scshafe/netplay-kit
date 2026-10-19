# 0001: Netcode model

Status: accepted, 2026-10-19.

## Context

The first user, ledgefall, is a platform fighter. Its simulation is
deterministic: a frame's state is a pure function of the previous state and the
frame's inputs, and client (arm64) and server (x86_64) must agree bit for bit
(ledgefall `docs/architecture.md`). Play is over the internet, starting on a
tailnet, and must feel responsive for tight timing.

## Decision

**Input-based rollback with an authoritative relay server.**

- Clients send inputs only, with a small input delay.
- The server fixes the canonical input for each frame. An input that arrives
  late becomes the player's previous input, so a slow client cannot stall the
  others.
- The server broadcasts the canonical inputs.
- Clients predict missing inputs, then roll back and re-simulate when a
  canonical input differs from the prediction.
- Periodic state hashes detect desyncs. The server's state wins on a resync.

The `Simulation` interface (step, snapshot, restore, hash, input encode and
decode) is the only thing a game provides.

## Alternative considered: server-authoritative snapshots with client prediction

The FPS-style model: clients send inputs, the server simulates and sends state
snapshots, clients predict locally and interpolate or reconcile.

- For: tolerates a non-deterministic simulation; a desync is impossible by
  construction; late joiners and spectators are simple.
- Against: bandwidth grows with state size, not input size; every correction
  is a visible state snap unless the game builds reconciliation for it; it
  gives up the advantage of a deterministic engine that already exists; the
  server must run a full simulation per match and the client a second copy.

Rejected because the simulation is deterministic by requirement, inputs are a
few bytes per frame, and rollback hides latency without visible correction.

## Consequences

- Games must be deterministic and able to snapshot and restore cheaply. A game
  that cannot do this is not a fit for this framework.
- The server is a relay and referee, not a second source of truth for every
  frame; it may also embed the simulation to check hashes and decide results.
- If a future game is non-deterministic, a snapshot model would be a new
  session type beside this one (a new record), not a change to it.
