# Benchmarking Methodology

Everything in this document is a rule about how numbers are produced, not a
number. The numbers live in `bench/results/`, next to the machine report that
makes them meaningful, and every one of them is reproduced by:

```bash
./scripts/bench_all.sh
```

If a number in the README cannot be produced by running that, it does not go
in the README.

---

## 1. Three different things, never conflated

| | What it measures | Tool | What it is *not* |
|---|---|---|---|
| **Per-operation cost** | one book or core operation, hot in cache | `bench/micro` (Google Benchmark) | latency — there is no queueing here |
| **End-to-end latency** | intended-send time → handler return, per command | `bench/harness/engine_bench` | throughput |
| **Throughput** | sustained commands/sec with the ring never starved | `engine_bench`, reported as achieved *and* offered | a latency claim |

A per-operation microbenchmark on a cache-hot book is an excellent A/B
instrument and a terrible absolute number. It is reported as ns/op and never
as "the engine's latency".

## 2. Timing

`core/timestamp.hpp` has the details and the caveats; the summary:

* **x86-64**: `rdtscp`, TSC frequency calibrated at start-up against
  `steady_clock` over ≥100 ms. Publishable runs require `constant_tsc` and
  `nonstop_tsc` in `/proc/cpuinfo`; without them the counter changes rate with
  the P-state and every percentile is fiction.
* **AArch64 (Apple Silicon)**: `cntvct_el0`, measured at **23,999,995 Hz on
  this machine — one tick is 41 ns**. That is coarser than the operation being
  measured. `CycleCalibration::coarse()` returns true, and `engine_bench`
  prints a five-line banner saying the run is development-only. Those numbers
  are usable for A/B comparison and are never published as absolute
  percentiles.
* **Anything else**: `steady_clock`, which is a function call into the vDSO
  and perturbs what it measures. Development only.

**The timer read overhead is measured and published, and nothing is ever
subtracted from a sample.** Subtracting an estimate of the measurement cost
turns a measurement into a model. Publishing the floor lets the reader do the
subtraction themselves if they want to, and lets them see when a p50 is
sitting on it.

## 3. Coordinated omission

This is the part most order-book benchmarks get wrong, so it is worth being
explicit about.

A **closed-loop** benchmark sends a command, waits for the response, and sends
the next. When the system stalls, the load generator stalls with it — it stops
offering load exactly when the system is least able to serve it. The stall
therefore never enters the distribution. Reported tail latencies from
closed-loop harnesses can be wrong by two orders of magnitude, and they are
wrong in the flattering direction.

The **open-loop** harness here:

1. Precomputes an arrival schedule: command *i* is intended to be sent at
   `t0 + i/R`.
2. Busy-waits until the intended time, then submits. Busy-wait, not sleep: a
   sleep quantises the schedule to the timer granularity and invents latency
   the engine did not cause.
3. Measures latency as **completion − intended send time**, not
   completion − actual send time. A backlog caused by a stall therefore shows
   up in full, on every command that was supposed to have been sent during it.
4. Counts a **schedule miss** whenever the producer was already late before it
   tried to send. A run with more than 1% misses is reported as *saturated*,
   with a warning printed next to the percentiles, because in that regime the
   distribution describes the harness rather than the engine.

`--closed-loop` runs the other way. Both curves are published side by side in
the results, because the comparison is more persuasive than either number.

**"Completion"** is the instant `engine.apply()` returns for that command, by
which point every outbound event it produced has been handed to the sink.
(Spec question 25.10.) Measuring at "last event written to the sink" is the
same instant for every sink in the repository; handler-return is the one that
is also well defined for a command that produces no events.

## 4. What every report must contain

Both the human summary and the CSV row carry all of it:

* count, achieved throughput, offered throughput, schedule misses, ring-full
  events, sink drops;
* **p50, p90, p99, p99.9, p99.99, max — always together**, plus the full
  percentile dump for plotting;
* flow mix (the *realised* mix, not the requested one), pattern, seed, warm-up
  depth;
* build type, git SHA, timing backend, tap state, and the engine's compile-time
  sizing constants.

**Never a mean without a distribution. Never a p99 without p99.9 and max.**
The mean is printed with a note telling the reader not to read it alone.

The realised mix is published rather than the requested one because they
differ. A mix whose removals outpace its additions cannot sustain a book: the
live order set drains, cancels get substituted with adds, and the run
benchmarks an almost-empty book. See `SyntheticFlow::realised()` and the
`ADrainingMixIsVisibleInTheRealisedNumbers` test.

## 5. Machine configuration

`scripts/machine_report.sh` captures CPU model, core count, governor, turbo
state, C-states, SMT, TSC flags, hugepage configuration, kernel command line
(for `isolcpus` / `nohz_full`), NUMA topology, compiler version, and the
engine's build constants. It is written into the results directory by
`bench_all.sh` and committed with them.

For a **publishable** run on Linux x86-64:

* pin the engine thread and the load generator to different physical cores on
  the same socket (`--producer-core` / `--engine-core`, which use
  `pthread_setaffinity_np`);
* `isolcpus` + `nohz_full` on those cores where available;
* `performance` governor, **turbo disabled** — turbo-off numbers are worse and
  more repeatable, and saying so is the point;
* transparent hugepages or explicit `MAP_HUGETLB` for the pool and ladder,
  with a measured with/without comparison;
* at least 3 runs; report the **median** run and the inter-run spread. Never
  the best run.

On macOS the harness reports that pinning failed rather than implying a
pinning that did not happen: `thread_policy_set` with an affinity tag is a
hint the scheduler may ignore, and the scheduler may move a thread between
performance and efficiency cores mid-run.

### Charts

`scripts/plot_results.py <results-dir>` produces two, and only two:

* **`latency_cdf.png`** — percentile against latency, **log x**. A linear x
  axis spends 95% of its width on the region between p0 and p50 and squashes
  the entire tail into the last few pixels, which is the opposite of useful.
* **`latency_percentiles.png`** — the same six percentiles side by side for
  every configuration, so a "2× faster" claim can be checked at the tail as
  well as at the median. It takes the **median** run where a configuration was
  repeated, because reporting the best run is cheating.

It warns if `machine.txt` is missing from the directory, because a chart
without its machine report is a picture of a number rather than a result.

## 6. The six required comparisons

`bench_all.sh` produces all six. Each exists to answer a question a skeptical
reader will actually ask.

1. **`NaiveBook` vs `FastBook`**, same flow, same machine — the headline
   before/after. The `NaiveBook` baseline is committed *first*, before any
   optimisation exists. An optimisation with no committed "before" number is a
   story, not a result.
2. **Tap OFF vs tap ON** — pre-empts "doesn't your dashboard pollute the
   numbers?" with a measurement rather than an assurance.
3. **Synthetic mix vs measured real ITCH mix** — because a benchmark on an
   invented flow mix measures an invented system.
4. **Ladder-resident vs overflow prices** — publishing your own worst case.
5. **Cold book vs warm book** (0 vs 100,000 resting orders) — the cache
   behaviour is completely different and only one of them is realistic.
6. **Open loop vs closed loop** — §3.

## 7. Status

| Number | State |
|---|---|
| `NaiveBook` vs `FastBook`, Linux x86-64 | **not run** — no Linux reference machine is available |
| Development-machine A/B ratios | **measured** — `bench/results/dev-m1max/`, and see the caveat below |
| Tap on/off delta | **measured**, development machine |
| Real ITCH flow mix | **not run** — no ITCH sample day has been downloaded |
| Flamegraph and `perf stat` | **not run** — both need `perf`, which needs Linux |

### The caveat that applies to every number in this repository

The development machine is an Apple M1 Max. Its architectural counter measures
**23,999,995 Hz — 41 ns per tick**. That is coarser than the operations being
measured, so:

* **throughput figures are meaningful** — they are counts over a whole run,
  and 41 ns of granularity on a multi-second measurement is nothing;
* **per-operation figures from Google Benchmark are meaningful** — it measures
  CPU time over millions of iterations, not one operation;
* **latency percentiles are not.** A p50 that reads "541 ns" on this machine
  is 13 ticks; a p50 that reads "124 ns" is 3. The quantisation is visible in
  the numbers, and `engine_bench` prints a banner saying so on every run.

So this repository publishes **ratios and throughputs**, and marks every
latency percentile as development-only. Nothing here should be quoted as "the
p99 of this engine".

### The second caveat: this machine was not quiet

`scripts/machine_report.sh` captures the load average and the top CPU
consumers, and the committed report shows a load average between 34 and 124 on
a ten-core machine, with one background process taking a full core throughout.

Throughput survives that reasonably well. **Tail latency does not**: a single
scheduler preemption is tens of microseconds and lands directly in p99.9. Any
percentile past p90 in this result set is describing the machine at least as
much as the engine.

This is why the rule about `isolcpus`, `nohz_full`, a pinned pair of physical
cores and a fixed frequency governor is not pedantry. Without it you are not
measuring your program.

### Reading `bench/results/dev-m1max/`

Look at `schedule_misses` before any percentile. At the offered rates used
here the open-loop runs are saturated, which the harness announces — in that
regime the distribution describes the harness, not the engine, and the
`service_time` rows (offered rate 0, dequeue-to-return) are the usable ones.

That is not a flaw in the runs; it is what an honest open-loop harness does
when you offer a machine more load than it can take. The alternative — quietly
reporting the percentiles anyway — is the thing this whole section exists to
prevent.
