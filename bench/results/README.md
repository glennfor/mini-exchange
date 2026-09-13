# `bench/results/`

Committed benchmark output. Each subdirectory is one invocation of
`scripts/bench_all.sh`.

| File | What |
|---|---|
| `machine.txt` | the machine's state at the time of the run, **including its load average** |
| `results.csv` | one row per run, with full provenance |
| `cdf_*.csv` | percentile dumps, for the latency chart |
| `micro.json` | Google Benchmark output |
| `*.png` | charts, from `scripts/plot_results.py` |

**A results directory without its `machine.txt` is not a result.** The CSV
rows carry the build, git SHA, timing backend, tap state, flow mix, seed and
pool sizing; the machine report carries everything about the hardware and its
state that the CSV cannot.

## How to read a row, in order

1. **`machine.txt`'s load average.** Above the core count means the tail
   percentiles describe the machine, not the engine. One scheduler preemption
   is tens of microseconds and lands directly in p99.9.
2. **`coarse_timer`.** `yes` means the timing source cannot resolve the
   operation being measured — 41 ns per tick on Apple Silicon — so the
   percentiles are quantised and only the throughput is trustworthy.
3. **`schedule_misses`.** More than about 1% of `commands` means the run was
   saturated: the load generator could not keep its own schedule, and the
   distribution describes the harness. `engine_bench` prints a warning; the
   CSV expects you to check.
4. **`measure`.** `end_to_end_latency` is intended-send-time to handler
   return. `service_time` is dequeue to handler return, recorded when there
   was no offered rate at all — a different quantity, and never to be quoted
   as latency.
5. **`max_orders`.** `FastBook`'s pool sizing. At the compile-time ceiling of
   4,194,304 it allocates nearly 400 MB, and comparing that against a
   lazily-allocating `NaiveBook` measures the allocator rather than the books.

## Current contents

`dev-m1max/` — the development machine: Apple M1 Max, macOS, Apple clang 17,
`-O3 -mcpu=native`. **Development-only numbers.** The counter ticks at 24 MHz
and the machine was under heavy load from unrelated desktop software
throughout; both facts are in `machine.txt` and neither is a footnote.

Use these for **ratios and throughputs**. There is no Linux x86-64 result set
yet, so this repository has no publishable latency percentiles at all, and
`README.md` says so in its first table.

Raw per-sample dumps are not committed (`bench/results/raw/` is gitignored) —
they are large, regenerable, and the percentile dump is what the charts need.
