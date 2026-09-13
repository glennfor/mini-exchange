# mini-exchange

A price-time-priority limit order book matching engine, a zero-copy Nasdaq
ITCH 5.0 feed decoder, and the benchmark and validation machinery needed to
believe the numbers — in modern C++20.

> **Status: under construction.** Milestone 0 (skeleton) is complete.
> No performance number appears in this README until it has been measured on
> a machine whose configuration is committed next to it. Where a number is
> not yet measured, this file says `TBD` and nothing else.

## What it is

```text
ITCH 5.0 file ──► feed/ decoder ──┬──► book/ MarketDataBook ──► LOBSTER validation
                                  └──► engine/ MatchingEngine ──► bench/ harness
                                                              └──► viz/ tap → server → UI
```

* `core/` — allocation-free primitives: SPSC ring, slab pool, open-addressed
  map, hierarchical bitset ladder, cycle counter.
* `book/` — `NaiveBook` (obvious, an in-repo oracle) and `FastBook`
  (ladder + intrusive FIFOs + pool). One C++20 concept, no virtuals.
* `engine/` — matching policy: price-time priority, all order types and TIFs,
  cancel/replace priority rules, self-trade prevention.
* `feed/` — ITCH 5.0 decode, big-endian, `mmap`'d, no allocation.
* `pyref/` — an independently written Python oracle for differential testing.
* `bench/` — open-loop, coordinated-omission-free latency measurement.
* `viz/` — a shared-memory tap the engine cannot be stalled by, plus a
  server and a UI that contain no matching logic.

## Results

| Metric | `NaiveBook` | `FastBook` |
|---|---|---|
| Throughput (cmd/s) | TBD | TBD |
| p50 / p99 / p99.9 / max | TBD | TBD |

Nothing is published here yet. See `docs/benchmarking.md` for the
methodology that will produce these, and `scripts/bench_all.sh` for the
single command that will reproduce them.

## Build

Requires CMake ≥ 3.24, Ninja, and a C++20 compiler (Clang ≥ 16 or GCC ≥ 12).
GoogleTest is fetched by CMake; nothing is vendored.

```bash
cmake --preset dev
cmake --build build/dev
ctest --preset dev
```

Other presets: `ubsan`, `asan` (Linux; see DD-006), `tsan`, `release`,
`release-tap`, `release-portable`.

## Scope boundaries

Deliberately **not** implemented, and explained in
`docs/design-decisions.md`: multi-threaded matching, network transport or
gap recovery, persistence and crash recovery, kernel bypass, auction/cross
matching, anything that is not a single-symbol equity book, and any
connection to real money.

## Licence

MIT. See `LICENSE`.
