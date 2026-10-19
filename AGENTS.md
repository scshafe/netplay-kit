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

<!-- scshafe-dev:begin landing -->
## Verify and landing

Managed by scshafe-dev: `dev adopt` and `dev update` refresh this section from `dev.toml`; change `dev.toml`, not these lines.

Before finishing, both of these must pass:

```sh
./scripts/verify
dev check .
```

How a change lands:

1. Work on a branch and open a PR.
2. Run the two commands above. If the repository is private, GitHub Actions does not run for it: verify locally and say in the PR what you ran. If it is public, wait for CI to be green.
3. Merge the PR with a merge commit, one change at a time: `gh pr merge <N> --merge --subject "Merge #<N>: <title>"`. Never squash or rebase (both are off on the repository), and pass `--subject`: `gh pr merge` does not make the `Merge #N: <title>` subject by itself.
<!-- scshafe-dev:end landing -->
