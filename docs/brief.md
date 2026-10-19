# netplay-kit: the owner's brief

The owner's request, 2026-10-18 (lightly edited for reading), made while
starting ledgefall:

> The game needs to be playable by multiple players over the internet
> connecting to the same server. If applicable and a good idea, I want to also
> consider turning the networking aspect used in this game into a framework
> that can be easily reused in future games, because I expect I'll likely be
> reusing the similar architecture later on. This framework would maybe be of
> benefit in including a home page and lobby and game server design.
>
> The server should be able to run on this Arch machine. If it's not too big
> of a speed sacrifice, maybe netplay-kit runs in Docker so that it can run on
> either easily.

The owner chose a separate framework project (three projects: ledgefall,
fighter-physics, netplay-kit).

## Fixed points

- **Reusable.** Nothing in it names ledgefall. A game plugs in its
  simulation through an interface:
  - step with a frame's inputs;
  - snapshot and restore state;
  - hash state;
  - encode and decode inputs.
- **C++ on the server side.** The game server, the lobby and the home page
  server are C++. Client-side, the library is plain C++ that a native app
  embeds; ledgefall's macOS app calls it through Swift's C++ interop.
- **Internet play.** Several players on different networks join the same
  server. It starts on the owner's tailnet: players get a Tailscale node
  share. Public hosting comes later and must need no redesign; for example,
  encryption must not assume the tailnet.
- **Three parts:**
  - a **home page**: what the game is, how to join;
  - a **lobby**: rooms, seats, ready and start, matchmaking later;
  - a **game server**: runs matches.
- **Docker.** The services ship as container images and deploy through
  scshafe's runner lane, on the Arch PC first (`host = "pc"`). On Linux a
  container costs essentially nothing in CPU or latency. On macOS, for local
  development, the same programs build and run natively.
- **Fast enough for a platform fighter.** UDP for the game traffic, with
  netcode suited to a deterministic simulation: rollback with input delay
  and server authority. The design records the model chosen.
