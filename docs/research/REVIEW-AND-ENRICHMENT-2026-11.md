# netplay-kit: project review and enrichment ideas (November 2026)

Status: **proposed; do not merge.** This is a menu for the owner to pick from. It changes
no code. Written by the project's agent on Claude Fable 5.2, at the owner's request of
2026-11-09.

## Summary

netplay-kit did what its brief asked. All five milestones in `docs/design.md` are built,
the tests pass, and ledgefall plays whole matches through it. The transport and the
rollback core are the strongest parts. The weakest parts are the ones that only show
once the stack is deployed and a real client is written against it.

Top findings:

1. **The lobby's write limit will refuse waiting players in ledgefall's deployed shape**
   (D1). The limit is per source address and counts `/me`, which clients poll. Behind
   `tailscale serve` every player arrives from 127.0.0.1 and shares one bucket.
   Reproduced on loopback; the effect on the live stack is inferred, not observed.
2. **A game server that restarts quickly leaves its rooms "playing" for ever and loses
   capacity in the lobby** (D2). Reproduced. Decision 0003 assumes the lobby notices the
   restart; with `restart: unless-stopped` it does not.
3. **Every completed match reports its result three seconds late** (D3). Reproduced:
   7.65 s from "playing" to "finished" for 4.5 s of play.
4. **The kit is hard to consume as a client.** ledgefall compiles the kit's sources by
   directory glob, reaches into its private `src` directory, wrote the lobby client twice
   (C++ bot and Swift app) and works around a missing confirmed-input feed to record
   replays.
5. **There is no plan past milestone 5**, no tagged release, and no reconnect: a Wi-Fi
   blip ends a match for that player.

Top five ideas, in the order I would do them:

1. A bug-fix batch for D1 to D5, each with a regression test (small).
2. A client-facing API pass: split library targets, a C++ lobby client, a
   confirmed-frame feed, and end-of-session reasons (medium). It deletes most of
   ledgefall's workarounds.
3. Reconnect and resume into a running match (large; a design decision).
4. Replays, desync forensics and basic observability (medium to large).
5. A netcode lab: soak runs under named network profiles with metrics, a conformance
   kit for any `Simulation`, and real coverage-guided fuzzing (medium).

Encrypted game packets and public hosting are not in the five because they wait on an
owner decision (section 2.5), not because they matter less.

## How this review was done

- Commit reviewed: `9e56cd2` (main, Merge #21, 2026-10-21). Nothing has landed since.
- Read: `AGENTS.md`, `docs/brief.md`, `docs/design.md`, the four decision records, both
  closed issues (#8, #19), the PR list, every public header, and the implementation of
  the session layer, the game server, the lobby, the HTTP server and client, and the
  transport's connection and server. I skimmed the JSON and HTTP parsers, the site code
  and the tests rather than reading every line.
- Ran: `./scripts/verify` (22 of 22 tests pass, 52.6 s, GCC 16.2.1, Debug with ASan and
  UBSan) and `dev check .` (clean). The last three CI runs on main are green.
- Reproduced three defects against the built executables on loopback. The script is in
  the appendix.
- Consumers: a subagent read the public repositories of ledgefall (`a9b5980`),
  fighter-physics (`eaff550`), lost-footing (`4cffd54`), blob-royale (`495e396`) and
  lightfight (`69d3c17`). I checked its main ledgefall claims myself (the CMake glob, the
  `/me` polling loop, the `serve.json` proxy, the error handling in `Online.swift`).
- Not done: I cannot reach the tailnet, so nothing here was observed on the live stack.
  I did not run ledgefall's app. Defects marked "by reading" were not reproduced.

Effort sizes used below: **S** is one PR, **M** is two or three PRs, **L** is a milestone
of four or more PRs and usually a decision record. For scale, the whole kit was built in
21 PRs.

---

# Part 1: Review

## 1.1 Scorecard

| Area | Grade | Why |
| --- | --- | --- |
| Transport (`npk::transport`) | A- | Stateless cookie handshake that never amplifies, bounded buffers everywhere, a simulated lossy link with a virtual clock. No pacing, no migration, no encryption (the last is declared). |
| Session and rollback (`npk::session`) | B+ | Correct and well tested under loss. Missing: reconnect, a confirmed-frame feed, end reasons, any version check between builds. One real bug (D3). |
| Game server (`npk::gameserver`) | B | Clean hand-off and token checks. Connection cap not tied to match capacity (D4), blocking DNS in the match loop (D5), almost no logging. |
| Lobby (`npk::lobby`) | B- | The API is small and careful. Two defects that matter when deployed (D1, D2), no routing by game, state lost on restart. |
| Home page | B+ | Strict CSP, no inline anything, text from a bounded config. ledgefall's stack does not configure it. |
| Security on the tailnet | B+ | Hostile-input discipline is consistent. Shared secret and plain HTTP are acceptable there and documented. |
| Security for public hosting | D | By design not built yet: no packet encryption or integrity, no TLS, no accounts, room and connection slots are cheap to exhaust. |
| Tests and verify | A- | Every layer has tests, including hostile peers and whole matches under loss. Gaps: deployed-shape failures, Docker builds, coverage-guided fuzzing. |
| Docs | B | The design document is accurate about how things work. It is stale at the edges and has no forward plan. |
| Packaging and consumer API | C | One monolithic library target, no install rules, no tags, no lobby client, layout is de facto API. |
| Operability | C+ | Containers are tight (read-only, no capabilities, uid 65534). No health command, no metrics, three log lines in total, images not published. |

## 1.2 Where it stands against its own plan

Done, as planned in `docs/design.md` "First milestones":

| Milestone | State | Evidence |
| --- | --- | --- |
| 1. Transport | Done | PRs #2 to #6; `tests/handshake_test.cpp`, `channels_test.cpp`, `clock_sync_test.cpp`, `sim_link_test.cpp` |
| 2. `Simulation` and rollback session | Done | PRs #9 to #11; `tests/session_test.cpp`, `desync_test.cpp`, `rollback_test.cpp` |
| 3. Game server, lobby, images | Done | PRs #14 to #17; `tests/e2e.sh` runs the real executables |
| 4. UDP over the tailnet | Done | Issue #8; measured table in `docs/design.md` |
| 5. Home page | Done | PR #18 |
| Room options (ledgefall's request) | Done | Issue #19, PR #20 |

Half-built, or promised and not started:

| Item | Where it is promised | State |
| --- | --- | --- |
| Encryption and integrity of game packets | `docs/brief.md:33-34` ("encryption must not assume the tailnet"), `docs/design.md:333`, decision 0002 | Not started. No decision record yet. |
| Matchmaking | `docs/brief.md:37` ("matchmaking later") | Not started. |
| Joining a match that has started | `docs/design.md:207` ("not supported yet") | Not started. `Match::add_player` refuses once running (`src/session/match.cpp:85`). |
| Showing the measured round trip before a match | `docs/design.md:297` ("the lobby may want to") | Not started. |
| Coverage-guided fuzzing | Decision 0002 line 19 ("libFuzzer where the compiler provides") | Seeded random-mutation tests only. There is no libFuzzer target in the tree. |
| A next milestone | nowhere | The plan ends at milestone 5. Version is still 0.0.1 with no tag. |

## 1.3 Defects

Severity is for the deployed ledgefall stack on the tailnet.

### D1. The write limit counts `/me` and is per source address (high)

- `src/lobby/lobby.cpp:166-174` limits every non-GET request by the peer's IP address, at
  2 a second with a burst of 20 (`include/npk/lobby/lobby.hpp:69-70`).
- `/me` is a POST (`lobby.cpp:220-225`) and is the call a waiting client repeats.
  ledgefall's app calls it once per local seat every 0.5 s (`AppModel.swift:285` in
  ledgefall `a9b5980`); its bot calls it every 200 ms (`server/bot.cpp:144-151`).
- ledgefall's stack puts `tailscale serve` in front of the lobby
  (`deploy/stack/serve.json`: proxy to `http://127.0.0.1:8080`), so the lobby sees every
  player as 127.0.0.1.
- **Reproduced on loopback:** two seats polling `/me` every 0.5 s got their first 429 on
  round 17 (about 8 s in), and 42 of 120 calls were refused over 30 s.
- ledgefall's Swift client throws on any status it did not expect (`Online.swift:137`),
  so I expect the waiting screen to fail. I did not run the app.
- The same proxy also meets the HTTP server's 32-connections-per-address cap
  (`include/npk/http/server.hpp:48`), which would cap held long-polls for all players
  together if clients moved to long-polling.
- Fix (S): do not count `/me` as a write (make it a GET, or exempt it), and key the
  bucket by seat secret when one is presented. Whether to trust a forwarded-for header
  from a loopback proxy is a security choice for the owner (section 2.5).

### D2. A fast game-server restart strands rooms and leaks capacity (high)

- The lobby fails a playing room only when its server's heartbeat is older than 20 s
  (`src/lobby/lobby.cpp:590-599`). A container that restarts in a second re-registers
  under the same key and the lobby never notices that its matches are gone.
- `assigned` is only decremented by a result (`lobby.cpp:538`), and capacity is
  `max_matches - max(matches, assigned)` (`lobby.cpp:435`), so each stranded room removes
  one slot until the lobby itself restarts.
- **Reproduced:** with one slot, a room stayed "playing" 40 s after the game server was
  killed and restarted; the server reported `"matches":0`; a new room got
  `"error":"no_server"`.
- The same leak follows a result that is dropped after 12 tries, a minute
  (`include/npk/gameserver/game_server.hpp:61-62`). A playing room has no time limit of
  its own in the lobby.
- Decision 0003 ("Consequences") says the lobby sees the registration lapse. That holds
  only for an outage longer than 20 s.
- Fix (S): send a boot id and the list of live room ids with each registration (the game
  server already has both for `/status`), and let the lobby fail rooms the server no
  longer holds.

### D3. A completed match lingers for the whole flush timeout (medium)

- After the game finishes, `Match::update` waits until every client has acknowledged the
  last frame or `flush_timeout` (3 s) passes (`src/session/match.cpp:68-73`).
- Acknowledgements arrive in `Inputs` messages, and `Match::on_message` ignores every
  message once the state is not `Running` (`match.cpp:147`). So the acknowledgement can
  never arrive and the timeout always runs, unless the client hangs up first.
- **Reproduced:** the toy match of 240 frames (4.0 s plus a 0.5 s start delay) took
  7.65 s from "playing" to "finished" in the lobby.
- Effect: results are 3 s late, the client's connection stays open 3 s after the game
  ended, and the server resends the tail of the match 60 times a second meanwhile.
- Fix (S): keep taking `canonical_ack` while finished. No test covers the flush today.

### D4. The UDP connection cap is not derived from match capacity (medium, by reading)

- `transport::ServerConfig::max_connections` defaults to 64
  (`include/npk/transport/server.hpp:32`) and `gameserver::run` never sets it
  (`src/gameserver/main.cpp:20-26`).
- `NPK_MAX_MATCHES` accepts up to 10000 (`main.cpp:22`) and a room has up to 16 seats.
  At the default of 16 matches, four-player games need exactly 64 connections, leaving
  none for the up to 64 unjoined connections the server also allows
  (`game_server.hpp:59`).
- A client refused here gets no reply (`src/transport/server.cpp:137-140`); it sees a
  five-second handshake timeout.
- Fix (S): compute the cap from `max_matches`, the seat limit and `max_unjoined`, or
  refuse a configuration that cannot fit.

### D5. Name resolution blocks the match loop (medium, by reading)

- `http::resolve` calls `getaddrinfo` (`src/http/client.cpp:162`), which blocks.
- The game server calls it from `update` while it cannot find or register with the lobby
  (`src/gameserver/game_server.cpp:307-309`, and `:341-342` resets the timer after each
  failed registration). Every match on that server stops for as long as the resolver
  takes.
- The lobby calls it while assigning a room (`src/lobby/lobby.cpp:452-455`).
- With Docker's resolver a miss is fast, so this is latent. A hung resolver would freeze
  play.
- Fix (S to M): resolve on a helper thread, or only accept literal addresses in the loop
  and resolve at start and on a slow timer off the hot path.

### Smaller ones (by reading)

| # | What | Where | Fix |
| --- | --- | --- | --- |
| D6 | A seated client that reports a very old `canonical_ack` is sent the newest snapshot again at every checkpoint (twice a second, up to 64 KiB), and it never counts against the client. Bounded and needs a valid token, so it is waste, not a hole. | `src/session/match.cpp:166`, `:272-274`, `:247-265` | Count catch-ups; refuse an ack that goes backwards. S |
| D7 | Rooms carry a `game` name but game servers do not register one and `pick_server` ignores it. A lobby shared by two games sends rooms to the wrong server, which refuses and is then avoided for 10 s. | `lobby.cpp:302-306`, `:427-444`, `:499-522` | Register and match on `game`. S |
| D8 | A client learns only a `DisconnectReason`. A wrong token, a full server, a finished match and a desync drop look the same, and a wrong `Join` is noticed only when the server times it out 5 s later. | `src/gameserver/game_server.cpp:225-233`, `src/session/client_session.cpp:35-42`, `:165-168` | Idea Q4. |
| D9 | A POST with no body and no `Content-Length` gets 411. `/me` and `/leave` take no body, so `curl -X POST` fails in a way that looks like a lobby fault. It cost me one wrong reproduction. | `src/http/http.cpp:165` | Treat a missing length on POST as zero, or document it. S |

## 1.4 Drift between the docs and the code

| Doc says | Code does |
| --- | --- |
| `docs/design.md:1`: "design (starting point)" | All five milestones are built. The title undersells it and there is no "what is next". |
| `design.md:13` and `:171` name an `npk-gameserver` container | There is no such executable. The game server is a library; the kit ships `toy_gameserver` and each game builds its own. The text further down says so. |
| `design.md:234-236`: "Planned PRs: ... the executables, images and a compose file with an end-to-end test" | All merged (#12 to #17). |
| Decision 0003: a restarted game server's rooms are marked failed | Only after a 20 s gap (D2). |
| Decision 0002 line 19 and `AGENTS.md`: fuzz the parsers, libFuzzer where available | Seeded random mutation inside ctest (`tests/wire_fuzz_test.cpp` and the `garbage()` cases). Useful, but not coverage-guided. |
| `include/npk/transport/types.hpp:11`: "a peer's Close always reads as Closed on the receiving side" | True for the server (`src/transport/server.cpp:160`). The client passes the peer's reason through (`src/transport/client.cpp:105`). |
| `docs/brief.md:29-30`: the macOS app calls the kit through Swift's C++ interop | ledgefall uses a C shim (`include/ledgefall/capi.h`). The brief is fixed text and this is the consumer's choice, but the kit's headers were never exercised through Swift interop. |

Nothing in the code contradicts the netcode model, the hand-off or the security rules.
The drift is at the edges.

## 1.5 Test and verify health

Good:

- 20 unit and integration tests plus two shell tests; all pass locally and on CI
  (Ubuntu and macOS).
- The session tests run whole matches over a simulated lossy network and compare every
  confirmed checkpoint with a straight replay of the canonical inputs
  (`tests/session_test.cpp:18`). That is the right property.
- Hostile peers are tested on both sides (`tests/desync_test.cpp:92`, `:158`), and the
  lobby and control API get random requests (`tests/lobby_test.cpp:475`).
- The end-to-end test checks that a secret never reaches a log (`tests/e2e.sh`).

Gaps:

| Gap | Why it matters |
| --- | --- |
| No test of the deployed shape: many players from one address, a game-server restart, a lost result | D1 and D2 passed every test. |
| No test of the end-of-match flush | D3 passed every test. `tests/gameserver_test.cpp:360` just runs 8 s and looks at the end. |
| CI never builds the Docker images | `docker/Dockerfile` compiles with its own `g++` lines, without `-Werror`, by a different file list than CMake. It can break without CI noticing. |
| No Release build in CI | The images are `-O2`; the tests are Debug only. |
| No coverage-guided fuzzing | The parsers face the network. |
| No long-run test | 16-bit packet and fragment sequence wrap, a one-hour match, and slow leaks are untested. |
| No performance numbers | Nobody knows how many matches one server holds, or what a 16-frame rollback costs through the kit's per-frame snapshot. |
| `CHECK` exits at the first failure with only the expression | Fine now; slow to diagnose a CI-only failure. |
| Base-image digests are refreshed by hand ("checked 2026-10-21") | They will go stale quietly. |

## 1.6 Architecture and API risks

1. **No reconnect and no migration.** A connection is bound to one address
   (`src/transport/server.cpp:154`), a seat is taken once, and a running match refuses
   new players. A NAT rebind or a Wi-Fi roam ends the match for that player after the
   10 s idle timeout. For a game played over home Wi-Fi and Tailscale this is the main
   risk to a session actually finishing.
2. **Builds are not checked against each other.** The wire has `kVersion`
   (`include/npk/transport/wire.hpp:38`), but `Start` carries no kit or game build id. A
   client app one version behind the server will connect, desync, be repaired five times
   and be dropped (`match.cpp:249-252`) with `ProtocolError`. A shipped macOS app and a
   redeployed server make this likely.
3. **The `Simulation` interface is narrow in ways games already feel.**
   - `step` cannot tell a first run from a re-simulation, and the client has no
     confirmed-frame callback. The server has one (`MatchConfig::on_canonical`,
     `include/npk/session/match.hpp:45`). ledgefall records replays inside its adapter
     and discards them after a resync to work around this.
   - Inputs are fixed-size, at most 255 bytes, at most 16 players
     (`include/npk/session/messages.hpp:51-54`).
   - A snapshot must fit one 64 KiB reliable message or the client cannot be repaired
     (`match.cpp:256-259`). ledgefall is under 16 KiB today.
   - The kit snapshots every simulated frame (`src/session/rollback.cpp:125-132`). Cheap
     for fighter-physics (0.7 µs), a real cost for a bigger state.
4. **The library is one target.** A client that wants only transport and session must
   also compile HTTP, JSON and the lobby. ledgefall cannot (its note says
   `std::from_chars` for doubles, `src/json/json.cpp:315`, is unavailable below
   macOS 26), so it globs `src/session`, `src/transport` and `src/util` and adds the
   private `src` directory to its include path. The kit's directory layout is now an
   interface nobody declared.
5. **No pacing.** `Connection::flush` can send up to 256 packets in one call
   (`src/transport/connection.cpp:148`); a 64 KiB resync leaves as one burst. Fine on a
   LAN or tailnet, a loss risk on a thin uplink.
6. **Single-threaded with per-connection system calls.** The HTTP server polls each
   connection separately every 5 ms (`src/http/server.cpp:229`,
   `apps/npk_lobby.cpp:62`) and `Server::poll_message` walks the map from the start for
   each message (`src/transport/server.cpp:193-203`). Right for dozens of matches. It
   will not scale to hundreds without work, and nothing measures where the limit is.
7. **State lives in memory.** A lobby restart drops every room and seat; a game-server
   restart drops every match (decision 0003 accepts this).
8. **Almost nothing is logged.** The game server logs "room assigned" and registration
   changes; the lobby logs "room playing on". Match ends, desyncs, resyncs, dropped
   clients and refused joins are counted in stats structs that nothing exposes
   (`GET /status` returns counts and room ids only, `game_server.cpp:113-121`). When a
   match goes wrong in the field there will be no record of it.
9. **`gameserver::run(factory)` takes no configuration**
   (`include/npk/gameserver/main.hpp:24`). A game that wants a different grace period or
   duration must copy `main.cpp`.

## 1.7 Rough edges

For a developer adding a game:

- No guide. The path is "read `examples/toy` and `docs/design.md`".
- No lobby client. ledgefall wrote one in C++ for its bot and one in Swift for the app,
  and decodes the token hex in two places.
- No tags, no changelog, no install rules. Consumers pin a commit.
- No C API. ledgefall wrote its own shim for Swift.
- The lobby pushes room changes by long-poll, but both ledgefall clients poll on a timer
  instead, because nothing wraps the long-poll for them.

For the operator (the owner):

- No health command in the images and no `curl`; ledgefall's compose file uses bash's
  `/dev/tcp` for the health check.
- Images are not published. ledgefall copies `npk_lobby` out of its own FetchContent
  build tree.
- No metrics and thin logs (1.6, item 8).
- ledgefall's stack does not set `NPK_SITE_FILE`, so the home page shows the kit's
  default text.

For a player:

- A dropped connection is final.
- No rematch. ledgefall's host opens a new room and the others scan the room list for it.
- Failures have no reason attached (D8).

## 1.8 What blocks the owner from getting value today

1. D1 and D2, in the deployed stack. Both are small fixes.
2. No reconnect. It decides whether an evening of matches over real home networks is fun.
3. Players need a Tailscale node share. Anything wider needs the public-hosting work,
   which starts with a decision.
4. Only one fleet game can use the kit (section 2.1). That is by design, and I recommend
   keeping it so, but it limits the return on the framework to ledgefall and games shaped
   like it.

---

# Part 2: Brainstorm

## 2.1 Who could use it

| Project | What it is | Fit |
| --- | --- | --- |
| **ledgefall** (with fighter-physics) | Deterministic fixed-point platform fighter, 2 to 4 players, 60 Hz, native macOS client | The intended user. Everything fits. Its workarounds are the best list of what to build next. |
| **lost-footing** | Browser game, local multiplayer on one keyboard, JS doubles | No fit without a browser transport and a WASM or JS client. Its design says online play "moved to the ledgefall project". |
| **blob-royale** | C++ server with WebSocket snapshots to a React client, 400 Hz simulation, 8 seats, float simulation on one platform, not MIT | No fit. It is the snapshot model that decision 0001 rejected for this kit, and it already has its own rooms. |
| **lightfight** | Real-life laser tag: ESP32 firmware, iOS HUD, Go match server, event-based | No fit. No simulation, no ticks, different language. |

So the honest position: netplay-kit is a kit for deterministic native games, with one
consumer. A second consumer would most likely be the owner's next native game on
fighter-physics. The ideas below are ranked with that in mind. Making the kit serve
browser or snapshot games is possible (B7, B8) but I rank it low.

## 2.2 Quick wins

Ranked. Each is one PR unless marked.

| # | Idea | Value | Effort | Risks and dependencies |
| --- | --- | --- | --- | --- |
| Q1 | **Fix D1 to D5 and D9**, each with a regression test (many players from one address; restart within the heartbeat; flush time; capacity arithmetic; a resolver that hangs). | High. Removes the two failures most likely to be hit on the live stack. | S each, M together | D1's proxy question is the owner's (2.5). D2 changes the registration body: old and new servers must both be accepted for one release. |
| Q2 | **Split the library**: `npk_transport`, `npk_session`, `npk_http_json`, `npk_lobby`, `npk_gameserver`, with `npk::client` as transport plus session. Parse JSON numbers without `from_chars(double)`. | High for ledgefall: its glob and private include go away, and it builds once on macOS, not twice. | S | Keep `npk::netplay_kit` as an alias for everything so the current pin still builds. |
| Q3 | **A C++ lobby client** (`npk::lobby::Client`): create, join, ready, leave, wait for a ticket by long-poll, and turn the ticket into a `ClientSessionConfig`. Use it in `toy_bot`. | High. Removes about 150 lines from ledgefall's bot and gives every future game the same start. | S | Depends on Q2 if the app is to link it. The Swift app keeps URLSession unless the owner wants it replaced. |
| Q4 | **Session outcomes the client can read**: a `JoinResult` and an `End` message with reasons (bad token, room full, match over with its outcome, dropped for desync, version mismatch), and a kit and game build id in `Join`. | High. Turns "Disconnected from the game server" into something a player can act on, and makes version skew loud. | S to M | A wire change: bump the session protocol and say so in a changelog. Touches hostile-input code, so it needs its own fuzz cases. |
| Q5 | **A confirmed-frame feed on the client** (`on_confirmed(frame, inputs)`) and an "is this a re-simulation" flag. | High for replays and for effects and sound. Removes ledgefall's adapter-side recording. | S | Adding a parameter to `Simulation::step` breaks the interface; a callback on `ClientSessionConfig` does not. |
| Q6 | **Logs and status that explain a bad match**: one line per match end (outcome, frames, per-seat missing inputs, desyncs, resyncs), one per desync and per dropped client, and per-match stats in `GET /status`. | High for the owner. Today a bad evening leaves no trace. | S | Never log tokens or secrets; the e2e test already checks that. |
| Q7 | **CI builds the images** and a Release configuration; a `--check` flag on `npk_lobby` and the game server for container health checks. | Medium. Closes a blind spot and removes ledgefall's `/dev/tcp` trick. | S | Adds a few minutes to CI. |
| Q8 | **Tag v0.1.0** with a changelog and a one-page protocol reference (transport, session, lobby API, control API, with their version numbers). | Medium. Consumers get something to pin and a place to read what changed. | S | The compatibility promise is the owner's (2.5). |
| Q9 | **Refresh `docs/design.md`** (title, the stale lines in 1.4, a "next" section from whatever the owner picks here) and add `docs/guide.md`: "your first game on netplay-kit", written from the toy and from what ledgefall had to learn. | Medium. | S | None. |
| Q10 | **Rematch in the lobby**: `POST /api/rooms/<id>/rematch` makes a new room with the same seats and options, and the finished room's view points to it. | Medium. Replaces ledgefall's room-list scan. | S | Options are immutable per room today; a rematch may want a new seed. Let the caller pass new options. |
| Q11 | **Route rooms by `game`** (D7) and count catch-up state transfers (D6). | Low now, needed for a shared lobby. | S | None. |
| Q12 | `gameserver::run(factory, config)` so a game can set match options without copying `main.cpp`. | Low. | S | None. |

## 2.3 Bigger bets

Ranked.

| # | Idea | Value | Effort | Risks and dependencies |
| --- | --- | --- | --- | --- |
| B1 | **Reconnect and resume.** A seat's token stays valid for the life of the match; a new connection with the token takes the seat back; the client is caught up with the state transfer that already exists (`Resync`); meanwhile the seat plays neutral input as it does now. | Highest for players. Most of the mechanism is already there: tokens, `resync`, neutral input for the absent. | M to L | A design decision: how long a seat is held, and whether the game or the kit decides what an absent player does (2.5). Needs Q4. Token replay matters more once it is reusable, which ties to B3. |
| B2 | **Replays and desync forensics.** The server writes each match's options, canonical inputs, checkpoint hashes and result; a `npk_replay verify` tool re-runs one through a game's factory; on a desync both snapshots are dumped beside the replay. | High. Gives the owner a way to answer "what happened in that match", and gives ledgefall one replay format on both sides. | M | Needs somewhere to write (a volume: the owner's call, 2.5). Needs Q5 for the client side. ledgefall's `replay.hpp` is a good starting shape. |
| B3 | **Authenticated encryption of game packets.** Key each connection from the handshake and bind it to the lobby's token. | Required before any public hosting, and promised by the brief. | L | Decision record and the kit's first dependency (2.5). Hostile-input code: needs a careful review and fuzzing. Adds bytes per packet; the 1200-byte budget has room. |
| B4 | **A netcode lab.** A headless runner that plays N scripted clients over `SimLink` under named profiles (LAN, tailnet as measured in issue #8, DERP relay at 90 ms, lossy Wi-Fi bursts, a roaming client) and writes rollback depth, stalls, input delay, missing-input frames and bandwidth as CSV. CI fails when a metric regresses. | High for quality. Today `grace_frames`, `max_lead_frames` and the delay rule are reasoned, not measured. | M | None. It also gives a model something to tune against (2.6). |
| B5 | **A conformance kit for `Simulation`.** A test helper a game calls with its factory: snapshot and restore round trips, hash sensitivity, the same result with and without rollback, a lossy four-player match, and a golden hash trace across platforms. | High for the next game; removes most of what ledgefall wrote in `tests/netplay_test.cpp` and its determinism trace. | M | The helper ships in the repository but is test code; keep it out of the client library. |
| B6 | **Coverage-guided fuzzing.** libFuzzer targets for the packet parser, session messages, HTTP and JSON; a one-minute run in CI and a longer scheduled run; a small committed corpus. | Medium to high. It is what `AGENTS.md` asks for. | S to M | Clang only; the macOS job or a new clang-on-Linux job. Scheduled runs use Actions minutes. |
| B7 | **Public hosting package.** B3, plus a TLS reverse-proxy recipe for the lobby, an invite code or sign-in, per-address caps on rooms and UDP connection slots, and a hardening review. | High if the owner wants players without Tailscale; none otherwise. | L | Owner decision first (2.5). The proxy and the host are owner-session work. |
| B8 | **Spectators and late join.** Observer seats that receive canonical frames after an initial state transfer. | Medium. Good for a couch audience and for debugging. | M | Builds on B1's catch-up path. |
| B9 | **Matchmaking.** `POST /api/queue` with a game and options; the lobby fills rooms. | Low to medium at the current player count; it is in the brief. | M | Only worth it with more players than one friend group. |
| B10 | **Quality surface for the HUD and pre-match.** `ClientSession::quality()` (round trip, jitter, delay, rollback depth, stalls) and a ping to the game server before readying, as `design.md:297` suggests. | Medium. | S to M | None. |
| B11 | **Rollback at larger state sizes.** Snapshot every k frames, reuse buffers, measure. | Low until a game with a big state arrives. | M | Do B4 first so there is a number to improve. |
| B12 | **Lobby persistence and a shared front door.** Rooms and results survive a restart; one lobby and home page lists several games. | Low now. | M to L | A storage dependency (decision). Needs Q11. |
| B13 | **A browser path**: a WebSocket or WebTransport `Link` and a WASM client build. | Would open the kit to browser games. Low with today's fleet: lost-footing sent its online play to ledgefall. | L | Dependency and scope decision (2.5). I recommend against it for now. |
| B14 | **A snapshot-replication session type** beside rollback (decision 0001 foresaw it). | Low. blob-royale is the only candidate and has its own stack. | L | I recommend against it. |

## 2.4 Workflow, fleet and the owner's experience

- **Pin bumps as requests.** When the kit tags a release, the agent files one
  `agent-request` on ledgefall with the changelog and the new pin. ledgefall's decision
  record already says needs go through the owner session; this is the same path in the
  other direction. Depends on Q8.
- **A stack template for "a netplay-kit game"** in the runner lane: lobby, game server,
  sidecar, health checks, the UDP grant for the tailnet policy. This belongs to the
  fleet repositories and the owner session; I can only supply the kit's side (published
  images, a health command, the list of environment variables).
- **Published images** for `npk-lobby` on a tag (GHCR), so a game builds only its own
  server. Needs a `packages` grant for the agent's identity, which is the owner's.
- **A play-session report.** With Q6 and B2 in place, the game server can write one
  summary per evening: matches, results, desyncs, stalls, the worst link. The owner
  reads one page, not logs.
- **A reference deployment check in CI**: bring up `compose.yaml`, run two bots through
  it, and tear it down. Catches D1-class and D2-class faults before a deploy.

## 2.5 Decisions that are the owner's

| # | Decision | Why it is yours | My recommendation |
| --- | --- | --- | --- |
| 1 | Is public hosting a goal for the next phase? | It decides whether B3 and B7 are next or later, and it brings accounts, abuse and a host. | Not yet. Do reconnect and the client API first; they pay off on the tailnet too. |
| 2 | Which crypto library, when B3 starts | Decision 0002 requires a record and it would be the first dependency. | A small audited one (libsodium or Monocypher), pinned. Never hand-rolled. |
| 3 | Reconnect policy | It changes what a match means: how long a seat is held, whether the match pauses, whether a result counts. | Hold the seat for the rest of the match, never pause, let the game's `Simulation` decide what neutral input means. |
| 4 | May the game server write files (replays, reports)? | It adds a volume to a container that is read-only today. | Yes, one bounded directory with a size cap. |
| 5 | The D1 fix: trust a forwarded-for header from loopback? | A security choice: a trusted header is spoofable if the lobby is ever reachable directly. | Do not trust headers. Key the limit by seat secret and stop counting `/me`. |
| 6 | Versioning promise | Whether an old app must keep working against a new server. | For now: no compatibility across minor versions, but a clear refusal (Q4), not a desync. |
| 7 | Scope: deterministic native games only? | It decides B13 and B14 and what "reusable" means for the fleet. | Yes, keep the scope. One model done well. |
| 8 | Player identity | Names today are free text per seat; stats or history need something durable. | Leave it until public hosting is decided. |
| 9 | One lobby for several games, or one per game | Affects Q11, B12 and the home page. | One per game until there is a second game. |

## 2.6 Where newer models could help

None of this puts a model in the 60 Hz path; I would not.

1. **Reviews like this one, on a schedule or before a release.** D1 to D3 were found by
   reading the state machines across two repositories and then running the executables.
   That kind of cross-component reading is what the tests miss.
2. **Model-based tests.** Write a small reference model of `Match` and `Rollback` and
   check the real ones against it under generated schedules. A model writes the
   reference and the generators; the test itself stays deterministic and in CI.
3. **Fuzz harnesses and triage** (B6): write the targets, minimise crashes, and turn each
   into a unit test.
4. **Desync forensics** (needs B2): given two snapshot dumps and a replay, find the first
   frame and field that differ and read the game's code for the cause. The game would
   supply a "describe this snapshot" hook.
5. **Tuning from the netcode lab** (needs B4): sweep the delay rule and the server's
   grace settings per profile and propose defaults with the evidence.
6. **Keeping consumers in step.** On each kit release, draft the consumer's pin-bump
   request with the exact changes it needs (2.4).
7. **Bot players for soak tests.** A model writes input policies offline (aggressive,
   idle, erratic, disconnecting); the bots replay them. Wider coverage than the single
   scripted input stream in `toy_bot`.
8. **The game-integration guide** (Q9), written from ledgefall's adapter and kept in step
   when the interface changes.

## 2.7 One ranked list

Value for the owner against effort, quick wins and bets together.

| Rank | Item | Size | Kind |
| --- | --- | --- | --- |
| 1 | Q1: fix D1 to D5 with regression tests | M | quick win |
| 2 | Q2 + Q3: split targets and a lobby client | S + S | quick win |
| 3 | Q4 + Q5: session outcomes, build ids, confirmed-frame feed | M | quick win |
| 4 | Q6: logs and status | S | quick win |
| 5 | B1: reconnect and resume | M to L | bet; decision 3 |
| 6 | B4 + B5: netcode lab and `Simulation` conformance kit | M + M | bet |
| 7 | B2: replays and desync forensics | M | bet; decision 4 |
| 8 | Q7 + Q8 + Q9: CI images, a tag, docs | S each | quick win |
| 9 | B6: coverage-guided fuzzing | S to M | bet |
| 10 | Q10: rematch | S | quick win |
| 11 | B3, then B7: encryption, then public hosting | L + L | bet; decisions 1 and 2 |
| 12 | B10, B8: quality surface, spectators | S to M each | bet |
| 13 | Q11, Q12, B9, B11, B12 | various | later |
| 14 | B13, B14: browser path, snapshot sessions | L each | not recommended |

---

## Appendix: how the three defects were reproduced

All on loopback with the executables built by `./scripts/verify` at `9e56cd2`. Nothing in
the repository was changed. Lobby and `toy_gameserver` were started as in `tests/e2e.sh`,
with `NPK_MAX_MATCHES=1` for D2.

**D1.** Create a two-seat room, join twice, then for each seat send
`POST /api/rooms/<id>/me` (empty body, the seat's bearer secret) every 0.5 s for 30 s.

```
status 200: 78
status 429: 42
first refusal at round 17 of 60 (0.5 s apart, 2 seats)
```

**D2.** Create a two-seat room, join and ready both seats, wait for "playing".
`kill -9` the game server and start it again at once. Wait 40 s.

```
before the restart the room is: playing
40 s after the restart the room is: playing
game server status: {"max_matches":1,"matches":0,"rooms":[]}
a new one-seat room, joined and readied: "state":"open", "error":"no_server"
```

**D3.** Run two `toy_bot`s through a 240-frame match (`--create 2 --frames 240`) and poll
`GET /api/rooms/<id>` every 50 ms.

```
playing -> finished took 7.65 s (play itself is about 4.5 s)
```

A first attempt at D1 used `curl -X POST` with no body and got 411 on every call; that is
D9, not the rate limit, and the numbers above are from the corrected run.
