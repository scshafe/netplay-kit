# The kit's example stack: the lobby, and a game server running the toy game.
# Build from the repository root:
#   docker build -f docker/Dockerfile --target lobby -t npk-lobby .
#   docker build -f docker/Dockerfile --target toy-gameserver -t npk-toy-gameserver .
#
# Both images hold one binary (libstdc++ linked in, libc from the Debian base so
# that name resolution works: a static glibc cannot resolve compose service names),
# run as uid 65534, and need no capabilities, volumes or published control port.
# A game builds its own server image the same way, with its own Simulation and
# `npk::gameserver::run`.
FROM gcc:13-bookworm AS build
WORKDIR /src
COPY include include
COPY src src
COPY apps apps
COPY examples examples
# g++ directly: the image's cmake is older than the project's 3.28.
RUN mkdir /obj && cd /obj \
    && find /src/src -name '*.cpp' | xargs -P 4 -n 1 sh -c 'g++ -std=c++20 -O2 -Wall -Wextra -I/src/include -I/src/src -c "$0" -o "$(echo "$0" | tr / _).o"' \
    && ar rcs /libnpk.a /obj/*.o
RUN g++ -std=c++20 -O2 -static-libstdc++ -static-libgcc -Wall -Wextra -I/src/include -I/src/examples/toy -o /npk-lobby /src/apps/npk_lobby.cpp /libnpk.a \
    && g++ -std=c++20 -O2 -static-libstdc++ -static-libgcc -Wall -Wextra -I/src/include -I/src/examples/toy -o /toy-gameserver /src/examples/toy/toy_gameserver.cpp /libnpk.a

FROM debian:bookworm-slim AS lobby
COPY --from=build /npk-lobby /npk-lobby
USER 65534:65534
EXPOSE 8080/tcp
ENTRYPOINT ["/npk-lobby"]

FROM debian:bookworm-slim AS toy-gameserver
COPY --from=build /toy-gameserver /toy-gameserver
USER 65534:65534
EXPOSE 7777/udp
ENTRYPOINT ["/toy-gameserver"]
