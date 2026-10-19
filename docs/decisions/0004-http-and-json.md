# 0004: HTTP and JSON for the lobby and the control channel

Status: accepted, 2026-10-19.

## Context

Decision 0002 expected the lobby to be the first place that asks for a library.
Both the lobby (browsers and the macOS app talk to it) and the control channel
between lobby and game servers need HTTP/1.1 with JSON bodies, as a server and as
a client, on Linux and macOS, in a container image, from C++20.

## Options

- **cpp-httplib** (MIT, header-only, one file) and **nlohmann/json** (MIT,
  header-only). Pinned to a tag, vendored or fetched, they would work. Costs:
  cpp-httplib is thread-per-connection with blocking sockets by default and
  large (several thousand lines, much of it TLS, multipart and compression we
  would not use), so most of what we audit is unused; the two together are a new
  dependency on both platforms, and the lobby is hostile-input code.
- **In-tree**, restricted to what the lobby needs:
  - HTTP/1.1, one request per connection (`Connection: close`), `Content-Length`
    bodies only (no chunked, no upgrade, no TLS, no compression), strict parsing
    with hard limits on line, header and body sizes, a header count limit, a
    deadline for the whole request, and a limit on open connections.
  - JSON: a recursive-descent parser with a depth limit, a size limit and no
    numbers beyond what a double and an int64 hold; a writer that escapes.
  - Non-blocking sockets driven by the same `update(now)` loop as the rest of the
    kit, so no threads.

## Decision

In-tree, restricted as above. It is small (well under 1500 lines together), it
is fuzzed and tested at its limits like the packet parser, it adds no dependency
and no licence to track, and the lobby needs none of the features that make the
libraries large. The server and client are behind small interfaces, so a pinned
library can replace them if the needs grow (long-lived push connections,
TLS): that would be a new record.

## Consequences

- No TLS. Termination is the reverse proxy's job (see 0003).
- One request per connection costs a handshake per call; the lobby's traffic is
  a few requests a second, and a push channel is done by long-polling.
- The parsers are ours to keep correct; they have their own fuzz tests.
