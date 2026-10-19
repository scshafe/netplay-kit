# 0002: Dependencies

Status: accepted, 2026-10-19.

## Context

netplay-kit is MIT-licensed, embedded in a native macOS app and shipped as
small server images. Network code is hostile-input code, and every dependency
widens what has to be audited and built on two platforms.

## Decision

- Use the C++20 standard library and the OS socket API (POSIX sockets on Linux
  and macOS). No third-party library is used now.
- Small primitives are written in-tree and tested against published vectors
  rather than pulled in. The first is SipHash-2-4 for handshake cookies,
  tested against the reference vectors from the SipHash paper.
- Test tooling is the toolchain's: CTest, and sanitizers (ASan, UBSan)
  and libFuzzer where the compiler provides them. No test framework library.
- Any third-party library (an HTTP server, JSON, crypto, a test framework)
  needs its own record in `docs/decisions/` that names the version, pins it
  (an exact tag or commit, never a branch), and states a licence compatible
  with MIT.

## Consequences

- No `FetchContent` or package manager is needed to build the transport.
- The lobby's HTTP and JSON needs (milestone 3) will probably be the first
  record that asks for an exception; it will compare a small in-tree server
  against a pinned library then.
- Authenticated encryption of game packets (design, Security) is not
  hand-rolled: when it is built, a vetted library or construction is chosen by
  a new record.
