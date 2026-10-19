# netplay-kit agent notes

- A reusable C++20 framework for online multiplayer: transport, rollback
  sessions, an authoritative game server, a lobby and a home page
  (`docs/brief.md` is fixed; `docs/design.md` is the designer's, kept in step
  with the code). It never names a specific game: games plug in through the
  `Simulation` interface. ledgefall is the first.
- Server parts ship as Docker images and deploy through the scshafe runner
  lane on the PC (`host = "pc"`), configured in the game's repository.
  Deploying, tailnet nodes and the UDP-over-tailnet check are the owner
  session's work: put what you need from the host in the PR and ask.
- Network code is hostile-input code. Bound every length, never amplify
  before a handshake, rate-limit, and fuzz the packet parsers.
- A third-party dependency needs a decision record (`docs/decisions/`), a
  pinned version, and a licence compatible with MIT.
- Public, MIT. CI runs on GitHub: wait for it before merging.
