#!/usr/bin/env bash
# Reproduces every published benchmark number in one invocation.
#
#     ./scripts/bench_all.sh [output-directory]
#
# Anything in the README or in docs/benchmarking.md that is a number must be
# produced by this script. If a number cannot be reproduced by running this,
# it does not go in the README.
#
# Writes into bench/results/<label>/:
#   machine.txt      -- scripts/machine_report.sh output
#   results.csv      -- one row per run, with full provenance
#   cdf_*.csv        -- percentile dumps for the latency chart
#   micro.json       -- Google Benchmark output
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

OUT_DIR="${1:-bench/results/$(date -u '+%Y%m%d-%H%M%S')}"
mkdir -p "$OUT_DIR"

COMMANDS="${MINIEX_BENCH_COMMANDS:-2000000}"
RATE="${MINIEX_BENCH_RATE:-2000000}"
REPEATS="${MINIEX_BENCH_REPEATS:-3}"

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

say "machine report -> $OUT_DIR/machine.txt"
bash scripts/machine_report.sh > "$OUT_DIR/machine.txt"
tail -5 "$OUT_DIR/machine.txt"

say "building release and release-tap"
cmake --preset release > /dev/null
cmake --build build/release --target engine_bench micro_bench engine_replay
cmake --preset release-tap > /dev/null
cmake --build build/release-tap --target engine_bench

CSV="$OUT_DIR/results.csv"
BENCH=./build/release/bench/engine_bench
BENCH_TAP=./build/release-tap/bench/engine_bench

run() {
  local label="$1"; shift
  for i in $(seq 1 "$REPEATS"); do
    say "$label (run $i/$REPEATS)"
    "$@" --commands "$COMMANDS" --csv "$CSV" \
         --cdf "$OUT_DIR/cdf_${label}_${i}.csv"
  done
}

# --- spec 18.6, the six required comparisons -------------------------------

# (1) NaiveBook vs FastBook -- the headline before/after, same flow, same
#     machine, same everything else. The NaiveBook row is the committed
#     "before": an optimisation with no recorded baseline is a story, not a
#     result.
for BOOK in naive fast; do
  run "${BOOK}-balanced"   "$BENCH" --book "$BOOK" --rate "$RATE" --seed 1 \
      --pattern balanced --warmup 10000
  # And the throughput view of the same thing (rate 0 -> service time; the
  # harness labels the rows so the two can never be confused).
  run "${BOOK}-throughput" "$BENCH" --book "$BOOK" --rate 0 --seed 1 \
      --pattern balanced --warmup 10000
done

# (2) tap OFF vs tap ON, on the fast book, which is the one that will ship.
run "fast-balanced-tap"    "$BENCH_TAP" --book fast --rate "$RATE" --seed 1 \
    --pattern balanced --warmup 10000

# (3) synthetic mix vs measured real ITCH mix.
#     The measured mix arrives with tools/flow_profile in M8; until then this
#     runs the synthetic default and the row is labelled as such.
run "fast-synthetic-mix"   "$BENCH" --book fast --rate "$RATE" --seed 1 \
    --mix add=45,cancel=40,replace=10,aggressive=5 --warmup 10000

# (4) ladder-resident prices vs overflow prices -- FastBook's known worst
#     case, published rather than avoided.
run "fast-ladder-resident" "$BENCH" --book fast --rate "$RATE" --seed 1 \
    --pattern touch-churn --warmup 10000
run "fast-ladder-overflow" "$BENCH" --book fast --rate "$RATE" --seed 1 \
    --pattern ladder-overflow --warmup 10000

# (5) cold book vs warm book.
run "fast-cold"            "$BENCH" --book fast --rate "$RATE" --seed 1 --warmup 0
run "fast-warm"            "$BENCH" --book fast --rate "$RATE" --seed 1 --warmup 100000

# (6) open loop vs closed loop, same flow, published side by side.
run "fast-closed-loop"     "$BENCH" --book fast --closed-loop --seed 1 --warmup 10000

# --- extra shapes, because a suite that only runs the pleasant case is
#     marketing ---------------------------------------------------------------
run "fast-deep-book"       "$BENCH" --book fast --rate "$RATE" --seed 1 \
    --pattern deep-book --warmup 10000
run "fast-wide-spread"     "$BENCH" --book fast --rate "$RATE" --seed 1 \
    --pattern wide-spread --warmup 10000

say "microbenchmarks -> $OUT_DIR/micro.json"
./build/release/bench/micro_bench \
  --benchmark_format=json \
  --benchmark_out="$OUT_DIR/micro.json" \
  --benchmark_out_format=json

say "done"
echo "results:  $CSV"
echo "machine:  $OUT_DIR/machine.txt"
echo "plot with: python scripts/plot_results.py $OUT_DIR"
