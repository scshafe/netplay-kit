#!/usr/bin/env bash
# Runs npk_echo's server and probe against each other on loopback: every size
# must come back, none lost.
set -euo pipefail
echo_bin="$1"
port=$((20000 + RANDOM % 20000))
out="$(mktemp)"
trap 'kill "$srv" 2>/dev/null || true; rm -f "$out"' EXIT
"$echo_bin" serve --port "$port" --duration 60 >/dev/null &
srv=$!
sleep 0.3
"$echo_bin" probe --host 127.0.0.1 --port "$port" --count 20 --interval-ms 5 --sizes 64,1200 | tee "$out"
# size sent recv loss%: both rows must show 20 received
awk '$1==64 || $1==1200 { if ($3 != 20) bad=1; n++ } END { exit (bad || n != 2) }' "$out"
