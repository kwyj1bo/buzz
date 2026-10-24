#!/usr/bin/env bash
# Benchmarks a Release build of buzz-server with buzz-bench on this machine
# and writes a Markdown report. Server and load generator share the machine,
# so the numbers are a lower bound for a dedicated server.
# Usage: scripts/bench.sh [bin_dir] [report.md]
set -euo pipefail

BIN=${1:-build/src}
OUT=${2:-bench-results.md}
WORK=$(mktemp -d)
RESULTS="$WORK/results.txt"
SERVER_PID=""
PORT=17100

cleanup() {
  [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true

start_server() {
  PORT=$((PORT + 1))
  "$BIN/buzz-server" --port "$PORT" --cert "$WORK/server.crt" --key "$WORK/server.key" \
    --gen-cert --db "$WORK/bench-$PORT.db" --message-rate 1000000000 \
    --message-burst 1000000000 --max-connections 50000 --log-level warn "$@" &
  SERVER_PID=$!
  for _ in $(seq 1 100); do
    (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && return 0
    sleep 0.1
  done
  echo "server did not start" >&2
  exit 1
}

stop_server() {
  kill "$SERVER_PID"
  wait "$SERVER_PID" 2>/dev/null || true
  SERVER_PID=""
}

# run LABEL SCENARIO [options]: runs buzz-bench and records its results.
run() {
  local label=$1
  shift
  echo "== $label: buzz-bench $*" >&2
  local extra=()
  [ -n "$SERVER_PID" ] && extra=(--port "$PORT" --ca "$WORK/server.crt" --server-pid "$SERVER_PID")
  "$BIN/buzz-bench" "$@" "${extra[@]}" |
    awk -v label="$label" '$1 == "RESULT" { print label, $2, $3, $4 }' | tee -a "$RESULTS" >&2
}

CORES=$(nproc)

start_server --pbkdf2-iterations 1000
run handshakes handshakes --clients 2000 --threads 8
run connections connections --clients 10000 --threads 8
run chat_max chat --rooms 50 --members 20 --senders 1 --window 8 --duration 10 --threads 4
run chat_steady chat --rooms 50 --members 20 --senders 1 --rate 20 --duration 10 --threads 4
run chat_heavy chat --rooms 50 --members 20 --senders 1 --rate 40 --duration 10 --threads 4
run sync sync --messages 1000
stop_server

start_server --threads 1
run logins_1_worker logins --clients 24 --threads 8
stop_server
start_server --threads "$CORES"
run logins_n_workers logins --clients 48 --threads 16
stop_server

run p2p p2p --messages 20000

value() { awk -v l="$1" -v k="$2" '$1 == l && $2 == k { print $3 }' "$RESULTS"; }
row() { printf '| %s | %s %s |\n' "$1" "$(value "$2" "$3")" "$4"; }

{
  echo "# Buzz benchmark results"
  echo
  echo "- Date: $(date -u +%Y-%m-%dT%H:%MZ)"
  echo "- Machine: ${CORES} vCPU ($(lscpu | awk -F: '/^Core\(s\) per socket/ { c = $2 } /^Thread\(s\) per core/ { t = $2 } END { gsub(/ /, "", c); gsub(/ /, "", t); print c " cores x " t " threads" }')), $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"
  echo "- Memory: $(free -g | awk '/Mem:/ { print $2 }') GiB, kernel $(uname -r)"
  echo "- Build: Release, $(${CXX:-c++} --version | head -1)"
  echo "- Server and load generator run on the same machine over loopback, with TLS 1.3."
  echo
  echo "| Metric | Result |"
  echo "| --- | --- |"
  row "TLS handshakes per second (2,000 clients)" handshakes handshakes_per_sec "/s"
  row "TLS handshake p50" handshakes handshake_p50 "µs"
  row "TLS handshake p99" handshakes handshake_p99 "µs"
  row "Concurrent logged-in connections" connections connections ""
  row "Server memory with all connections open" connections server_rss_mib "MiB"
  row "Server memory per connection" connections rss_per_connection_kib "KiB"
  row "Ping round trip p50 with 10k connections open" connections ping_rtt_loaded_p50 "µs"
  row "Ping round trip p99 with 10k connections open" connections ping_rtt_loaded_p99 "µs"
  row "Max chat throughput, messages in (1,000 clients, 50 rooms)" chat_max chat_messages_per_sec "/s"
  row "Max chat throughput, deliveries out (fan-out x20)" chat_max chat_deliveries_per_sec "/s"
  row "Latency p50 at max throughput" chat_max chat_latency_p50 "µs"
  row "Latency p99 at max throughput" chat_max chat_latency_p99 "µs"
  row "Steady load: messages in" chat_steady chat_messages_per_sec "/s"
  row "Steady load: deliveries out" chat_steady chat_deliveries_per_sec "/s"
  row "Steady load: end-to-end latency p50" chat_steady chat_latency_p50 "µs"
  row "Steady load: end-to-end latency p99" chat_steady chat_latency_p99 "µs"
  row "Steady load: server CPU" chat_steady server_cpu_percent "%"
  row "Heavy load: deliveries out" chat_heavy chat_deliveries_per_sec "/s"
  row "Heavy load: end-to-end latency p50" chat_heavy chat_latency_p50 "µs"
  row "Heavy load: end-to-end latency p99" chat_heavy chat_latency_p99 "µs"
  row "Heavy load: server CPU" chat_heavy server_cpu_percent "%"
  row "Replay of 1,000 missed messages on rejoin" sync replay_ms "ms"
  row "Logins per second, 1 worker (PBKDF2 600k)" logins_1_worker logins_per_sec "/s"
  row "Logins per second, ${CORES} workers (PBKDF2 600k)" logins_n_workers logins_per_sec "/s"
  row "Ping p50 during the login storm" logins_n_workers ping_rtt_during_logins_p50 "µs"
  row "Ping p99 during the login storm" logins_n_workers ping_rtt_during_logins_p99 "µs"
  row "Server CPU cores busy during the login storm" logins_n_workers server_cpu_cores_busy ""
  row "P2P encrypted UDP round trip p50" p2p p2p_rtt_p50 "µs"
  row "P2P encrypted UDP round trips per second" p2p p2p_roundtrips_per_sec "/s"
  echo
  echo "<details><summary>All raw results</summary>"
  echo
  echo '```text'
  cat "$RESULTS"
  echo '```'
  echo
  echo "</details>"
} > "$OUT"
echo "wrote $OUT" >&2
