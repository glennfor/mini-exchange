# mini-exchange

A price-time-priority limit order book matching engine, a zero-copy Nasdaq
ITCH 5.0 feed decoder, and the benchmark and validation machinery needed to
believe the numbers — in modern C++20.

> **Status: under construction.** Milestones 0-5 are complete: build
> skeleton, core primitives, a correct matching engine, an independent Python
> oracle with differential testing, the open-loop benchmark harness, and
> `FastBook`.
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

## Correctness

Three independent implementations of the matching semantics agree, byte for
byte, on the canonical event stream:

| | What it is |
|---|---|
| `NaiveBook` | `std::map` of price to `std::list`; obvious, never optimised, never deleted |
| `FastBook` | price ladder, intrusive FIFOs, slab pool, hierarchical best-price bitset |
| `pyref/` | written from `docs/matching-semantics.md`, not from the C++; talks to the engine only through text files |

Checked by `engine_replay --book both --diff` over every flow pattern
including the pathological ones, and by `pyref/differential.py`, which
minimises any disagreement to a 1-minimal reproducer before reporting it.
The differential suite also injects known bugs and asserts they are caught —
one of which was not, which is how the flow generator's missing coverage of
same-price size-downs was found.

## Results

| Metric | `NaiveBook` | `FastBook` |
|---|---|---|
| Throughput (cmd/s) | TBD | TBD |
| p50 / p99 / p99.9 / max | TBD | TBD |

**Nothing is published here yet.** No Linux x86-64 reference machine has been
run. The development machine is Apple Silicon, whose architectural counter
ticks at 24 MHz — 41 ns per tick — which cannot resolve the operations being
measured; the harness prints a banner saying so on every run.

See `docs/benchmarking.md` for the methodology and `scripts/bench_all.sh` for
the single command that reproduces every number.

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
