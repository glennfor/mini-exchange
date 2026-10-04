# mini-exchange

A price-time-priority limit order book matching engine, a zero-copy Nasdaq
ITCH 5.0 feed decoder, and the benchmark and validation machinery needed to
believe the numbers — in C++20, with an independent Python oracle and a live
dashboard that cannot perturb the engine it watches.

```text
ITCH 5.0 file ──► feed/ decoder ──┬──► book/ MarketDataBook ──► LOBSTER validation
                                  └──► engine/ MatchingEngine ──► bench/ harness
                                                              └──► viz/ tap → server → UI
```

---

## Highlights

| | |
|---|---|
| **1.44× faster** than the baseline book on realistic mixed flow — 21.0 M vs 14.6 M cmd/s | and the baseline is kept, tested and benchmarked rather than deleted |
| **Cancel costs the same at the head, middle and tail** of a 4096-deep level — 60.5 / 60.1 / 60.1 ns | the measurement that justifies the whole design; real flow is mostly cancels |
| **Three independent implementations agree byte for byte** | two C++ books and a Python oracle written from the spec rather than the code; 1,000 sequences × 1,000 commands against each on every push |
| **1,390× faster best-price discovery** — 3.60 ns vs 5,004 ns | three-level hierarchical bitset against a linear scan of the same bits |
| **105 M ITCH messages/s, 3.0 GiB/s** decoded zero-copy | and 233,505,582 fuzz executions over 19 minutes with no crash and no RSS trend — the run is recorded in `docs/itch-notes.md` |
| **Book reconstruction checked after every single message** | against an independently written reconstruction, ten levels a side, including 876 crossed or locked states |
| **A live dashboard that cannot perturb the engine**, measured | killing the reader mid-run made the producer *faster*: 14,069 µs → 2,426 µs over identical work |
| **Open-loop benchmarking**, with coordinated omission demonstrated | the same work measured closed-loop reports a p99.9 22× better, and the chart shows both |

Integer tick prices throughout — a CI job greps `core/`, `book/`, `engine/`
and `feed/` and fails the build on a `float` or a `double`, because a price
that cannot be compared exactly is a price that matches non-deterministically.

---

## Correctness

Three implementations of the matching semantics agree, byte for byte, on the
canonical event stream:

| | What it is |
|---|---|
| `NaiveBook` | `std::map` of price to `std::list`; obvious, never optimised, never deleted |
| `FastBook` | price ladder, intrusive FIFOs, slab pool, hierarchical best-price bitset |
| `pyref/` | written from `docs/matching-semantics.md`, not from the C++, and connected to it only through text files |

```bash
# in-repo: the same engine over both books, every flow pattern
./build/release/tools/engine_replay --synthetic 200000 --book both --diff \
    --pattern ladder-overflow --max-orders 1048576

# cross-language: the Python oracle against the compiled engine
python -m pyref.differential --engine build/release/tools/engine_replay \
    --book fast --seeds 1000 --length 1000
```

A disagreement is minimised to a **1-minimal** reproducer before it is
reported: binary search on the prefix, then delta-debugging chunk removal,
then single-command removal repeated to a fixpoint. Removing any remaining
command makes the failure disappear.

**The differential suite also injects known bugs and asserts they are caught.**
One of them was *not*: "size-down keeps time priority" survived, because the
flow generator re-randomised the price on every replace and therefore almost
never produced a replace at the order's own price. The one replace rule that
keeps priority was going untested by a suite that had been green for every
seed tried. That is the argument for mutation testing in one paragraph.

---

## Results

Every figure here is an **A/B measurement**: same build, same flow, same run
length, same machine, one variable changed. That is what they are for, and the
ratios are the result.

The machine is an Apple M1 Max, whose architectural counter ticks at 41 ns —
coarser than a single operation — so the *absolute* percentiles are
development figures rather than quotable ones. `engine_bench` detects the
counter and prints a banner on every run saying so; quotable latencies come
from x86-64 Linux with an invariant TSC. The machine report is committed
beside the data in `bench/results/dev-m1max/machine.txt`, and every CSV row
carries the build, git SHA, timing backend, tap state, flow mix, seed and pool
sizing.

### Per-operation cost (Google Benchmark, median of 3, `-O3`)

| Benchmark | | |
|---|---:|---|
| Realistic mixed flow, `NaiveBook` | **14.6 M** cmd/s | the committed "before" |
| Realistic mixed flow, `FastBook` | **21.0 M** cmd/s | **1.44×** |
| `FastBook`, ladder-resident prices | 22.2 M cmd/s | its best case |
| `FastBook`, ladder-overflow prices | 9.8 M cmd/s | **its worst case — slower than `NaiveBook`** |
| Cancel at the **head / middle / tail** of a 4096-deep level | 60.5 / 60.1 / 60.1 ns | identical, which is the whole claim |
| One partial fill (no teardown at all) | 17.4 ns | |
| Replace, same price (keeps priority) | 9.7 ns | |
| Replace, crossing price | 98.2 ns | |
| Next occupied level, hierarchical bitset | **3.60 ns** | |
| Next occupied level, linear scan of the same bits | **5,004 ns** | **1,390×** |
| Pool allocate + free | 1.52 ns | |
| Hash find (hit / miss) | 2.97 / 1.06 ns | dense sequential ITCH ids |
| SPSC ring, 64-element batch | 0.36 ns / item | |
| ITCH decode, `switch` dispatch | **105 M** msg/s, 3.0 GiB/s | what ships |
| ITCH decode, 256-entry function-pointer table | 98.7 M msg/s | 0.94× |
| ITCH decode, computed goto (GNU extension) | 120 M msg/s | 1.15×, but not allowed in a header |
| Decode one Add Order message | 1.61 ns | |
| 48-bit big-endian load, unaligned | 0.32 ns | one instruction |

Cancel being **identical at the head, the middle and the tail** of a
4096-deep price level is the intrusive doubly-linked list doing its job, and
it is the single measurement that most justifies `FastBook` existing — real
flow is dominated by cancels and they do not arrive in queue order.

The overflow row is the other one worth looking at. `FastBook`'s resident
ladder is O(1); prices outside its 131,072-tick window fall into a
`std::map`, and on flow that lives there it runs at **44% of its own best
case** and slower than the book it replaces. That is a real limitation of
this design and it belongs in the results table rather than a footnote. See
`docs/design-decisions.md` DD-022.

### End-to-end, open loop

Reproduce everything below with one command:

```bash
./scripts/bench_all.sh
```

| Configuration | throughput | p50 | p99 | p99.9 | max | sched. misses |
|---|---:|---:|---:|---:|---:|---:|
| `NaiveBook`, balanced, tap off | 1.60 M/s offered | 458 ns | 7.2 µs | 48.0 µs | 308 µs | 0.68% |
| `FastBook`, balanced, tap off | 1.60 M/s offered | 520 ns | 11.9 µs | 102 µs | 381 µs | 0.76% |
| `FastBook`, tap **on** | 1.60 M/s offered | 499 ns | 14.3 µs | 770 µs | 841 µs | 1.02% |
| `FastBook`, **ladder-overflow** | 1.60 M/s offered | 666 ns | **3.14 ms** | 5.98 ms | 6.25 ms | 0.78% |
| `FastBook`, cold book (0 resting) | 1.60 M/s offered | 500 ns | 11.3 µs | 50.1 µs | 361 µs | 0.76% |
| `FastBook`, warm book (100k resting) | 1.60 M/s offered | 500 ns | 14.6 µs | 122 µs | 425 µs | 1.06% |

Service time only (no offered rate — dequeue to handler return, a *different
quantity*, never to be quoted as latency):

| | throughput | p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| `NaiveBook` | 3.84 M cmd/s | 83 ns | 499 ns | 1.67 µs |
| `FastBook` | 3.75 M cmd/s | 166 ns | **416 ns** | **1.29 µs** |

![Latency CDF](bench/results/dev-m1max/latency_cdf.png)

*Percentile against latency, log x, with the percentile axis compressed
towards 100 so each decade of nines gets equal space. The dashed line is the
closed-loop run — see below.*

![Latency percentiles by configuration](bench/results/dev-m1max/latency_percentiles.png)

### The one number worth the whole harness

Same engine, same flow, same machine, same run length — only the load
generator differs:

| | p50 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|
| **Open loop** (latency from the *intended* send time) | 520 ns | 11.9 µs | **102 µs** | 381 µs |
| **Closed loop** (send, wait, send) | 291 ns | 1.0 µs | **4.5 µs** | 79 µs |

The closed-loop harness reports a p99.9 **22× better** than the open-loop one
on identical work. Neither is lying about what it measured; the closed loop
simply stops offering load exactly when the system is least able to serve it,
so the queueing it causes never enters its own distribution. **This is
coordinated omission**, and it is the dashed line sitting an order of
magnitude to the left of everything else in the chart above. Any order-book
benchmark that does not say which of these two it ran has not told you its
most important number.

### Three things these numbers say that are not flattering

1. **`FastBook`'s tail is worse than `NaiveBook`'s here** (p99 11.9 µs against
   7.2 µs) even though its *service time* tail is better (416 ns against
   499 ns) and its microbenchmark throughput is 1.68× higher. The likely cause
   is its memory footprint — 40 MB of pool, index and ladder against
   `NaiveBook`'s lazily-grown few — on a machine that is already thrashing.
   That is a hypothesis, not a finding; it needs a quiet machine to settle.
2. **The overflow path is 263× worse at p99** than the resident path. See the
   per-operation table above.
3. **The tap costs a real amount**: p99.9 goes from 102 µs to 770 µs with it
   compiled in. That is why it is a compile-time option, why every result row
   carries its state, and why the harness exits rather than printing a
   "tap ON" number with the tap silently off.



### What the harness refuses to do

* **No mean without a distribution.** There is no API to ask for p99 alone;
  `summary()` always carries p50, p90, p99, p99.9, p99.99 and max together.
* **Nothing is subtracted.** The timer's own read overhead is measured and
  published so a reader can see the floor, and left in every sample.
* **Saturation is announced.** A run whose schedule misses exceed 1% prints
  `SATURATED: these percentiles describe the harness` next to the numbers,
  because in that regime they do.
* **A "tap ON" run with the tap off exits** rather than printing a number.

`docs/benchmarking.md` has the full methodology, including why the open-loop
arrival schedule matters and what coordinated omission does to a closed-loop
harness.

---

## Validation

`MarketDataBook` reconstructs the exchange's book from the message stream and
is compared against a published book **after every single message** at ten
levels on both sides — not at a coarser granularity, because a reconstruction
can be wrong for ten thousand messages and right again by the time anyone
looks.

```bash
./build/release/tools/lobster_check \
    --messages replay/tests/data/SYNTH_2026-09-13_34200_57600_message_10.csv \
    --book     replay/tests/data/SYNTH_2026-09-13_34200_57600_orderbook_10.csv \
    --levels 10
# VALIDATION PASSED: 1000 messages, 1000 book rows compared at 10 levels per
# side, zero mismatches
```

The fixture's orderbook file was produced by `pyref/lobster_check.py`, an
independently written Python reconstruction — so this is a genuine
cross-implementation check, the same shape as the engine's three-way
agreement. It deliberately contains **876 crossed or locked states across
1000 messages**, because that is the thing a naive reconstructor dies on:

> The no-crossed-book invariant is a property of the **matching engine**, not
> of a market data feed. A replace can move an order through the opposite
> side, and the book stays crossed until the other side reacts. Real feeds do
> this on a normal day.

The fixture is synthetic. Pointing the same check at a real LOBSTER day is two
commands:

```bash
./scripts/fetch_data.sh lobster
./build/release/tools/lobster_check --messages data/lobster/..._message_10.csv \
                                    --book data/lobster/..._orderbook_10.csv
```

`docs/validation.md` records exactly what has been run against what, and why
the ITCH↔LOBSTER cross-check needs a paid sample (the free days from the two
sources do not overlap in date).

---

## Divergence: ITCH flow through our engine

**ITCH is a market data feed. It is not an order entry stream.** It reports
what Nasdaq already matched. You cannot replay it through a matching engine
and expect identical fills, and a project claiming to is either confused or
being dishonest.

What `tools/inject` does instead: take the `A`/`F` adds as an **order arrival
process**, apply `X`/`D`/`U` by reference, and compare the trades *our* engine
generates against the `E`/`C`/`P` executions in the feed — then name and
quantify every difference.

The tool runs; the table it fills in is left empty on purpose. The only ITCH
file here is synthetic, with adds and executions generated independently and
no matching logic connecting them, so its "62.8% unexplained" measures the
generator rather than the engine — a number worth less than no number.
`docs/divergence.md` has the divergence classes it accounts for — hidden
liquidity, auction crosses, odd lots, publication-vs-arrival ordering,
routed-away liquidity — and the one command that produces the table from a
real day.

---

## The dashboard

```bash
./scripts/demo.sh --mock     # no C++ build, no market data
./scripts/demo.sh            # engine → shm tap → server → browser
```

There is no screenshot here: the development environment's only browser window
was occluded, which freezes a page's timers, so every capture came back from a
renderer that was alive but not drawing. A mock-up would have been worse than
the two commands above.

The UI is verified by mounting the whole dashboard in jsdom against a
stubbed WebSocket and feeding it the committed sample recording — byte for
byte the file the mock server replays — asserting the canvases issue draw
calls, the tape renders newest-first, transport commands reach the socket, and
**zero frames are rejected**. That last assertion is the drift check between
the TypeScript and Python halves of the wire schema, which otherwise fails as
a silently blank panel rather than an error.

**The latency panel is empty under `demo.sh`, and says so.** `tap_producer`
publishes zeros for the percentiles on purpose — the matching core does not
time itself, because measuring latency is the benchmark harness's job and an
engine that instruments its own hot path is measuring the instrumentation.
The panel names `engine_bench` rather than showing "0 ns", which would be a
measurement that was never taken, and a flattering one.

The tap's contract is about what it does to the *engine*:

* the engine's write is one bounded store into shared memory — no syscalls, no
  locks, no allocation, and a full ring increments a drop counter and returns;
* **killing the reader mid-run does not perturb the producer**, measured:
  14,069 µs with a live reader, 2,426 µs over identical work after the reader
  was killed. Losing the reader makes the producer *faster*, never slower;
* the tap's own cost is published, not waved away: 4.27 M cmd/s with
  `MINIEX_TAP=OFF` against 3.39 M with it on, everything else identical;
* Python cannot drain a C++ engine at a million commands a second and does not
  pretend to. Past a configured backlog the reader **skips forward** and counts
  what it skipped, rather than showing a lagging replay while claiming to be
  live. The count is published and the UI shows it in red.

---

## Build and run

Requires CMake ≥ 3.24, Ninja, and a C++20 compiler (Clang ≥ 16 or GCC ≥ 12).
GoogleTest, Google Benchmark and HdrHistogram are fetched by CMake; nothing is
vendored.

```bash
cmake --preset release && cmake --build build/release
ctest --preset release                     # 284 tests

# the Python oracle and the viz server
python3.11 -m venv .venv && .venv/bin/pip install -e '.[dev]'
.venv/bin/python -m pytest                 # 113 tests

# the UI
cd viz/ui && npm install && npm test       # 52 tests
```

Verified from a fresh `git clone` on the development machine (Apple M-series,
Apple Clang 17): **13 s to configure and fetch the three dependencies, 18 s to
build, 3 s to run all 284 C++ tests**, and the Python suite green against the
clone with no virtualenv. That check is worth running rather than assuming —
it is how the missing LOBSTER fixture in the "what I got wrong" list below was
found, and the numbers above are re-measured rather than carried forward.

Presets: `dev`, `ubsan`, `asan`, `tsan`, `fuzz`, `release`, `release-tap`,
`release-portable`.

### Tools

| Tool | What |
|---|---|
| `engine_replay` | run commands through the engine; `--record`/`--verify` for determinism, `--book both --diff` for the in-repo differential |
| `engine_bench` | the open-loop latency harness |
| `itch_stats` | message-type histogram, time span, decode throughput |
| `book_dump` | reconstruct a symbol and print the top N levels |
| `lobster_check` | the message-by-message LOBSTER validation |
| `tape_print` | the trade tape, with provenance per print |
| `flow_profile` | the empirical flow mix and order-lifetime distribution |
| `inject` | ITCH order flow into the engine, and the divergence table |
| `tap_producer` | the engine writing into a shared-memory tap |
| `tap_layout_dump` | the tap's binary layout, which the Python reader cross-checks against |

---

## Bugs worth reading about

Every number above survived a measurement that was wrong first. Five of the
twenty-eight, chosen because the fix is the boring half:

* **The cycle counter silently returned garbage on Apple Silicon.**
  `asm("isb; mrs %0, cntvct_el0")` assembles to the `isb` alone, because `;`
  starts a comment in Apple's integrated assembler. The "timestamps" were
  whatever was in the register. A monotonicity test caught it on the first
  run — which is why the counter has one.

* **Two microbenchmarks were measuring Google Benchmark.** `CancelHead`
  reported 98 µs per cancel. The real figure is 28 ns; the other 99.97% was
  `PauseTiming`/`ResumeTiming` rebuilding the book every iteration. A
  benchmark that never looks implausible is a benchmark nobody has read.

* **The dashboard showed a book the engine could not have held** — bids at
  100.2195 against asks at 100.1840, crossed by 355 ticks and stuck that way
  for minutes. The server reconstructs depth from a *delta* stream, and the
  tap is lossy by design, so one lost cancellation leaves a level resting for
  ever. Pause the transport for four seconds at 200k events/s and it goes
  from 2,949 resting orders to 6,313 and never recovers. A delta book cannot
  survive a gap; the repair is to discard it and rebuild, not to patch around
  it.

* **The test that exists to prevent a vacuous pass passed vacuously.** The
  zero-allocation suite replaces the global `operator new` and counts calls;
  its first test allocates on purpose to prove the replacement is wired up.
  It used a *new-expression*, which `[expr.new]/10` lets the compiler elide —
  and at `-O2` clang does. Writing the guard in a form the optimiser can
  delete is the same mistake the guard exists to catch, one level down.

* **The heatmap's scroll buffer was being used as a store.** Whatever a blit
  pushes past an edge is destroyed. That is correct going left, where time
  moves one way; the price axis moves both ways, so history that drifted off
  the top came back as a blank stripe. Two wrong fixes preceded the right one,
  and the right one was never in the window arithmetic at all.

The full set — twenty-eight, with what made each one invisible — is in
[`docs/postmortem.md`](docs/postmortem.md).

## Scope

Single-symbol equity matching, and deliberately nothing else. Not implemented,
each with its reasoning in `docs/design-decisions.md`: multi-threaded matching
(the core is single-threaded by design; the right way to add concurrency is
multi-symbol sharding, not locks), network transport, multicast, gap recovery
and FIX order entry, persistence and crash recovery, kernel bypass, auction
and cross matching (`Q` messages are consumed and reported, never replicated),
anything that is not a single-symbol equity book, and any connection to real
money.

Two things the machine, rather than the code, is waiting on:

* **Quotable latencies need x86-64 Linux with an invariant TSC.** The harness,
  the pinning flags, the TSC checks and `scripts/machine_report.sh` are all
  committed; this machine's 41 ns counter is what stops the absolute numbers
  being quotable. The ratios do not depend on it.
* **The real-data runs need a paid sample.** `fetch_data.sh` points the
  LOBSTER validation and the ITCH divergence table at a real day; the free
  days from the two sources do not overlap in date, which is why the
  cross-check between them is the one thing `docs/validation.md` lists as
  outstanding.

Next, in order: the Linux run; a real ITCH and LOBSTER day; the `FastBook`
overflow path, the one place it loses to the book it replaces (263× at p99),
where a second coarser ladder should restore the O(1) property; the
`PriceLevel` padding question (DD-021), where the variable exists purely so
the measurement can be taken and a negative result would be published; and a
committed flamegraph, `perf stat`, and dashboard screenshot.

## Documentation

| Document | What |
|---|---|
| `docs/architecture.md` | module boundaries, threading model, wire formats |
| `docs/matching-semantics.md` | the behavioural contract, and the oracle's source |
| `docs/design-decisions.md` | 37 decisions: context, options, choice, measured consequence |
| `docs/benchmarking.md` | the methodology, and what the harness refuses to do |
| `docs/validation.md` | what has and has not been validated |
| `docs/divergence.md` | why ITCH replay is not what it sounds like |
| `docs/itch-notes.md` | the protocol's traps, and the dispatch measurements |
| `docs/postmortem.md` | all 28 bugs this project found in itself, and what hid each one |

## Licence

MIT. See `LICENSE`.
