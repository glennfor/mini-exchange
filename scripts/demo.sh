#!/usr/bin/env bash
# One command, a working demo.
#
#     ./scripts/demo.sh
#
# Starts the engine writing into a shared-memory tap, starts the viz server
# reading it, starts the UI, and tells you where to look. Ctrl-C stops all
# three.
#
# With --mock it skips the engine entirely and replays the committed 30-second
# recording, which needs no C++ build and no market data:
#
#     ./scripts/demo.sh --mock
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MOCK=0
RATE=200000
SHM="/miniex_demo"
UI_PORT=5199
WS_PORT=8765

while [ $# -gt 0 ]; do
  case "$1" in
    --mock) MOCK=1 ;;
    --rate) RATE="$2"; shift ;;
    --ui-port) UI_PORT="$2"; shift ;;
    --ws-port) WS_PORT="$2"; shift ;;
    -h|--help)
      cat <<'USAGE'
demo.sh -- one command, a working demo.

  ./scripts/demo.sh            engine -> tap -> server -> UI
  ./scripts/demo.sh --mock     replay the committed recording; no C++ build,
                               no market data
  ./scripts/demo.sh --rate N   commands per second the engine emits
  ./scripts/demo.sh --ui-port N --ws-port N
USAGE
      exit 0
      ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

PIDS=()
cleanup() {
  echo
  echo "stopping..."
  for pid in "${PIDS[@]:-}"; do
    [ -n "$pid" ] && kill "$pid" 2>/dev/null
  done
  wait 2>/dev/null
  exit 0
}
trap cleanup INT TERM

python_bin() {
  if [ -x .venv/bin/python ]; then echo .venv/bin/python
  elif command -v python3.11 >/dev/null; then echo python3.11
  else echo python3
  fi
}
PY="$(python_bin)"

port_free() {
  ! lsof -nP -iTCP:"$1" -sTCP:LISTEN >/dev/null 2>&1
}

for port in "$UI_PORT" "$WS_PORT"; do
  if ! port_free "$port"; then
    echo "port $port is already in use. Pass --ui-port / --ws-port." >&2
    exit 2
  fi
done

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

if [ "$MOCK" -eq 0 ]; then
  say "building the engine with the tap compiled in"
  cmake --preset release-tap > /dev/null
  cmake --build build/release-tap --target tap_producer

  say "starting the engine -> $SHM at $RATE commands/s"
  ./build/release-tap/tools/tap_producer \
      --shm "$SHM" --capacity 1048576 --commands 0 \
      --warmup 20000 --rate "$RATE" &
  PIDS+=($!)
  sleep 1

  say "starting the viz server on ws://127.0.0.1:$WS_PORT"
  "$PY" -m viz.server.server --shm "$SHM" --port "$WS_PORT" --symbol DEMO \
      --build-sha "$(git rev-parse --short HEAD 2>/dev/null || echo unknown)" &
  PIDS+=($!)
else
  say "replaying the committed recording (no engine, no market data)"
  "$PY" -m viz.server.server \
      --mock viz/server/sample_frames.jsonl.gz --port "$WS_PORT" &
  PIDS+=($!)
fi
sleep 1

say "starting the UI"
if [ ! -d viz/ui/node_modules ]; then
  echo "installing UI dependencies (first run only)"
  (cd viz/ui && npm install)
fi
(cd viz/ui && npx vite --port "$UI_PORT" --strictPort) &
PIDS+=($!)
sleep 3

cat <<NOTE

================================================================
  Open http://127.0.0.1:$UI_PORT

  Things to try, in order:

    1. Watch the heatmap fill from the right. Each column is one
       30 Hz publish tick; colour is log-scaled resting size.
    2. Hit PAUSE, then STEP. Step advances exactly one event --
       it is the control worth building first, because "what did
       that one message do to the book" is the question you ask
       over and over.
    3. Watch the ladder flash on every change and decay over
       ~300 ms.
    4. Look at the latency CDF's x axis. It is logarithmic, so
       p50 and p99.99 are in the same picture.
    5. Check the Engine panel for tap drops. Non-zero is shown in
       red and in words -- a dashboard that quietly shows a
       partial market is worse than one that admits it.

  Ctrl-C to stop everything.
================================================================

NOTE

wait
