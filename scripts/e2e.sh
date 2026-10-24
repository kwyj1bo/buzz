#!/usr/bin/env bash
# Runs the real server and two CLI clients on loopback, then checks that a
# room message and a peer-to-peer direct message both arrive.
# Usage: scripts/e2e.sh [bin_dir]
set -euo pipefail

BIN=${1:-build/src}
PORT=${BUZZ_E2E_PORT:-17000}
WORK=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$WORK"' EXIT

"$BIN/buzz-server" --port "$PORT" --cert "$WORK/server.crt" --key "$WORK/server.key" \
  --db "$WORK/buzz.db" --gen-cert --pbkdf2-iterations 1000 --log-level warn &
for _ in $(seq 1 50); do
  [ -s "$WORK/server.crt" ] && break
  sleep 0.1
done
sleep 0.5

client() { "$BIN/buzz" --port "$PORT" --ca "$WORK/server.crt"; }

{
  sleep 0.5; echo "/register alice password123"
  sleep 0.5; echo "/login alice password123"
  sleep 0.5; echo "/join general"
  sleep 2;   echo "hello from alice"
  sleep 1;   echo "/dm bob psst, this one is direct"
  sleep 2;   echo "/quit"
} | client > "$WORK/alice.log" 2>&1 &
alice=$!

{
  sleep 0.5; echo "/register bob password123"
  sleep 0.5; echo "/login bob password123"
  sleep 0.5; echo "/join general"
  sleep 5;   echo "/quit"
} | client > "$WORK/bob.log" 2>&1 &
bob=$!

wait "$alice" "$bob"

check() {
  if ! grep -qF "$2" "$WORK/$1.log"; then
    echo "FAIL: $1 did not see: $2"
    echo "--- alice ---"; cat "$WORK/alice.log"
    echo "--- bob ---"; cat "$WORK/bob.log"
    exit 1
  fi
}

check bob "alice: hello from alice"
check bob "[dm] alice: psst, this one is direct"
check alice "* direct channel to bob ready"
echo "e2e: room message and direct message delivered"
echo "--- bob's terminal ---"
cat "$WORK/bob.log"
