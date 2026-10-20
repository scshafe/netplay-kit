#!/usr/bin/env bash
# End to end, with the real executables: a lobby, a game server that registers
# with it, and two bots that create/find a room, take seats, ready, join the game
# server over UDP with their tokens and play a toy match to its end.
set -euo pipefail
bin="$1"   # the build directory
work="$(mktemp -d)"
pids=()
cleanup() {
  for p in "${pids[@]:-}"; do kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  rm -rf "$work"
}
trap cleanup EXIT

base=$((20000 + (RANDOM % 20000)))
lobby_port=$base; control_port=$((base + 1)); udp_port=$((base + 2))
export E2E_SECRET="$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')"
export NPK_CONTROL_SECRET_VAR=E2E_SECRET

# No secret, no start.
if env -u E2E_SECRET "$bin/npk_lobby" 2>/dev/null; then echo "lobby started without a secret"; exit 1; fi
if E2E_SECRET=short "$bin/toy_gameserver" 2>/dev/null; then echo "game server started with a short secret"; exit 1; fi

NPK_BIND=127.0.0.1 NPK_HTTP_PORT=$lobby_port "$bin/npk_lobby" 2>"$work/lobby.log" & lobby=$!; pids+=("$lobby")
NPK_UDP_PORT=$udp_port NPK_CONTROL_PORT=$control_port NPK_CONTROL_BIND=127.0.0.1 NPK_MAX_MATCHES=2 \
  NPK_LOBBY=127.0.0.1:$lobby_port NPK_ADVERTISE_CONTROL=127.0.0.1:$control_port NPK_ADVERTISE_UDP=127.0.0.1:$udp_port \
  "$bin/toy_gameserver" 2>"$work/gameserver.log" & gs=$!; pids+=("$gs")
sleep 0.5

room="e2e-$RANDOM"
timeout 90 "$bin/toy_bot" --lobby 127.0.0.1:$lobby_port --room "$room" --create 2 --frames 240 --name alice --timeout 60 >"$work/alice.out" 2>&1 & a=$!
sleep 1
timeout 90 "$bin/toy_bot" --lobby 127.0.0.1:$lobby_port --room "$room" --name bob --timeout 60 >"$work/bob.out" 2>&1 & b=$!

status=0
wait "$a" || status=1
wait "$b" || status=1
cat "$work/alice.out" "$work/bob.out"
grep -q "RESULT outcome=completed frames=240" "$work/alice.out" || status=1
grep -q "RESULT outcome=completed frames=240" "$work/bob.out" || status=1

# The servers stop cleanly on SIGTERM, and neither logged the secret or a token.
kill -TERM "$lobby" "$gs"
wait "$lobby" || { echo "lobby exit code $?"; status=1; }
wait "$gs" || { echo "game server exit code $?"; status=1; }
pids=()
if grep -q "$E2E_SECRET" "$work"/*.log "$work"/*.out; then echo "the secret is in a log"; status=1; fi
if [ "$status" -ne 0 ]; then
  echo "--- lobby.log"; cat "$work/lobby.log"; echo "--- gameserver.log"; cat "$work/gameserver.log"
fi
exit "$status"
