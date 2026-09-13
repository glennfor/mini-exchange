# Design Decisions

One entry per decision: context, options considered, the choice, and the
measured consequence where one exists. Entries are appended as the decision
is actually faced, not written speculatively.

A number in the "consequence" field is either a measurement with a
reproduction command, or the literal word `TBD`. It is never a guess.

---

## DD-001 — Prices are integer ticks, never floating point

**Context.** Every price in the system is compared for exact equality
(level lookup), ordered (price priority), and used as an array index
(ladder). Matching must be deterministic across compilers and optimisation
levels.

**Options.**
1. `double` dollars — what most toy order books do.
2. A fixed-point wrapper class over `int64_t`.
3. A bare `int64_t` typedef at 1e-4 dollars.

**Choice.** (3). `using Price = std::int64_t;` with `kPriceScale = 10000`.

**Why.** `double` cannot represent `0.01` exactly, so two orders "at the
same price" may compare unequal; worse, `-ffast-math` or a different FMA
contraction setting changes results between builds, which destroys the
determinism guarantee in §9.5 of the spec. A wrapper class buys type safety
at the cost of a great deal of operator boilerplate and, in debug builds,
non-trivial call overhead on the hot path; the typedef plus a CI grep
(`scripts/check_no_floats.sh`) gets most of the safety for none of the cost.
1e-4 is the native scale of both Nasdaq ITCH and LOBSTER, so decoding is a
widening integer load with no conversion at all.

**Consequence.** `scripts/check_no_floats.sh` fails the build if `float` or
`double` appears in `core/`, `book/`, `engine/` or `feed/` outside tests.

---

## DD-002 — `Price` is signed

**Context.** The ladder is indexed by `(price - base) / tick`, which is
negative for prices below the base.

**Choice.** `std::int64_t`.

**Why.** An unsigned subtraction that goes negative wraps to a colossal
positive value and then indexes out of bounds — with `-Wsign-conversion` on,
unsigned would also force casts at every arithmetic site. Signed overflow is
UB, which means UBSan *catches* a bad price computation instead of silently
producing a wrong index. Using the type whose misuse a sanitizer can detect
is worth more than the one extra bit of range, which no equity price needs.

---

## DD-003 — `PostOnly` is a time-in-force, not an order type

**Context.** The spec asks for this decision explicitly.

**Choice.** `enum class TimeInForce { Day, IOC, FOK, PostOnly }`.

**Why.** `OrderType` answers "how is this order's price expressed?" — limit
or market. `TimeInForce` answers "under what conditions may this order
rest?" — Day rests indefinitely, IOC never rests, FOK requires a complete
fill, PostOnly requires that resting be possible at all. PostOnly is plainly
the second question. Keeping it out of `OrderType` also preserves the
property that `type == Market` is exactly equivalent to "price is a
sentinel", which the matching loop relies on.

**Rejected.** A separate `bool post_only` flag: it makes the illegal state
`Market + post_only` representable, and the type/TIF matrix in
`docs/matching-semantics.md` is then no longer exhaustive.

---

## DD-004 — Market sell sentinel is `min() + 1`, not `min()`

**Context.** A market buy is a limit at +∞ and a market sell a limit at −∞,
so that `crosses()` needs no special case for market orders. But `min()` is
already taken by `kInvalidPrice`.

**Choice.** `kInvalidPrice = min()`, `kMarketSellPrice = min() + 1`,
`kMarketBuyPrice = max()`.

**Why.** `best()` on an empty side must return something that fails every
`crosses()` test, and `min()` does that for a buy but *succeeds* for a sell
— so "no price" and "any price, selling" must be different values. One tick
above the minimum is as good as −∞ for any real equity price and keeps the
sentinels adjacent and greppable. Asserted in `core/types.hpp`.

---

## DD-005 — Pool indices, not pointers, in `Order`

**Context.** `Order` must fit in one 64-byte cache line and must be
serialisable into the observability tap without translation.

**Choice.** `std::uint32_t` indices into a slab, `kInvalidIndex = 0xFFFFFFFF`.

**Why.** Three reasons, in order of importance: (a) half the size of a
pointer, and the intrusive list needs two of them per order, so this is 8
bytes of a 64-byte budget saved; (b) the slab can be moved or mapped at a
different address in the reader process without fixing up links, which is
what makes the shared-memory tap possible; (c) an index is bounds-checkable
in debug builds, a pointer is not. Index 0 is a valid order — the sentinel
is all-ones, not zero — because wasting slot 0 to make zero mean "null" is a
real capacity cost at 4M orders and buys nothing.

---

## DD-006 — Sanitizer coverage is split between local and CI

**Context.** The spec requires ASan+UBSan clean. On the primary development
machine (macOS 26 / Apple Silicon, Apple clang 17 and Homebrew clang 20)
*any* AddressSanitizer-instrumented binary hangs before reaching `main` —
reproducible with a five-line `printf` program, so it is an environment
defect, not a project one.

**Choice.** Keep the `asan` preset (it is what CI runs on
`ubuntu-latest`), and add a `ubsan` preset that works locally. Local
development runs `dev`, `ubsan` and `tsan`; ASan coverage comes from CI on
every push.

**Why.** The alternative — dropping ASan from the presets because it does
not run on one machine — would lose the coverage entirely. Documenting the
split is more honest than a preset that silently never runs.

**Reproduce.** `printf 'int main(){}' > t.c && cc -fsanitize=address t.c -o t && ./t`
hangs indefinitely on this machine; the same with `-fsanitize=undefined`
exits immediately.

---

## DD-007 — Cached opposite counters in the SPSC ring

**Context.** The ingress ring sits between the load generator and the
matching core, on two different physical cores. A naive implementation puts
`head` and `tail` next to each other and reads the opposite counter on every
operation.

**Choice.** Each counter gets its own 128-byte block, and each side keeps a
*private, non-atomic* cached copy of the other side's counter, refreshed only
when the ring appears full (producer) or empty (consumer).

**Why.** Without the split, every push dirties the line the consumer is
reading, and the line ping-pongs between cores at roughly 100 ns a round
trip — which would be the dominant cost in a system whose whole point is
sub-microsecond handling. Without the *cached* copy, the split alone is not
enough: the producer would still read the consumer's live line on every push.
The cached value can only be stale in the safe direction (the producer may
believe the ring is fuller than it is, never emptier), so a stale read costs
at most one wasted refresh, never a correctness bug.

128 bytes rather than 64: Apple Silicon's L2 line is 128 bytes, and x86
prefetches line pairs, so 64-byte separation still allows adjacent-line
interference.

**Consequence.** TBD — the with/without measurement belongs in M4, when the
harness exists to make it honestly.

---

## DD-008 — Backward-shift deletion rather than tombstones

**Context.** The `OrderId -> pool index` map is the hottest lookup in the
system, and real equity flow is dominated by cancels, so erases are as
frequent as inserts.

**Options.** (a) tombstones plus a rehash when the tombstone fraction passes
a threshold; (b) backward-shift deletion, which restores the invariant that
a probe run is contiguous.

**Choice.** (b).

**Why.** Tombstones make `erase` trivially cheap and then charge for it
later, all at once, in a rehash that touches the entire table. That is a
latency cliff of milliseconds in the middle of a trading day — the single
worst thing that can happen to a p99.9. Backward-shift deletion pays a
short, bounded shift loop on each erase (expected length under 1 at a 0.5
load factor) and never has a cliff. A system that is judged on its tail
should prefer bounded work every time to unbounded work occasionally.

---

## DD-009 — Fixed-point, not floating point, for cycles-to-nanoseconds

**Context.** `CycleCalibration` must convert counter ticks to nanoseconds,
which is naturally a fractional ratio.

**Choice.** Q32 fixed point in a `std::uint64_t` (nanoseconds per tick,
scaled by 2^32), with the multiply widened to `unsigned __int128` where
available.

**Why.** Partly the project-wide no-floats rule in `core/` — a `double`
multiply in the measurement path is one more thing that can differ between
builds. Mostly, though, it is that fixed point makes the *precision* of the
conversion explicit and auditable: Q32 resolves 2.3e-10 ns per tick, which
is four orders of magnitude finer than the counter itself, so the conversion
contributes nothing to the error budget and the whole error is the hardware's.
With a `double` you would have to reason about it; here you can read it off.

---

## DD-010 — Matching lives in the engine, storage lives in the book

**Context.** The obvious place to put `match()` is on the order book. Most
"order book in C++" repositories do exactly that.

**Choice.** The book owns storage, price ordering, FIFO ordering within a
level, and level aggregates, and nothing else. All matching policy — which
orders cross, what price a trade prints at, what a replace does to priority,
what a fill-or-kill pre-scan counts — lives in `MatchingEngine`.

**Why.** Policy and mechanism. But the concrete payoff is differential
testing: `NaiveBook` and `FastBook` are two implementations of the *same
mechanism*, so running one engine over both must produce byte-identical event
streams, and any difference is a storage bug. If matching lived in the book
there would be two implementations of the policy as well, and agreement
between them would prove much less — the two could agree on a shared
misreading of section 8.

The second payoff is that the book stays testable on its own terms: "does a
cancel keep the level aggregate correct" is a question with an answer that
does not mention trades.

---

## DD-011 — A crossing replace is matched by the same code path as a new order

**Context.** Spec question 25.3: where does a replace that crosses the book
get matched, and which path assigns the sequence numbers?

**Choice.** `on_replace` removes the resting order, emits `Replaced`, and
then calls the same private `accept()` that `on_new` calls after validation.
There is exactly one matching loop in the engine and it assigns every
sequence number.

**Why.** A second matching path is a second place for the price-time rules to
be subtly different, and the difference would only show up in the one case
nobody writes a directed test for. It also means the replace inherits, for
free and by construction, every property the new-order path has: trades print
at the resting price, the residual rests at the tail, the sequence numbers
are contiguous.

**Consequence.** The event order for a crossing replace is `Replaced` then
`Trade`s, which is what `ReplacedEventPrecedesTheTradesItCauses` pins down.
The alternative ordering (trades first, then a `Replaced` describing the
residual) was rejected because it would report a priority change that had
already been consumed.

---

## DD-012 — A tagged union, not `std::variant`, for commands

**Context.** `Command` is pushed through the SPSC ring by the millions.

**Choice.** An explicit `CommandType` discriminant plus an anonymous union.

**Why.** Three concrete reasons. (a) `std::variant` is only trivially
copyable when every alternative is, and its layout is not guaranteed, so
`memcpy`ing one into a shared-memory ring is not something the standard
promises will work. (b) `std::visit` is commonly a function-pointer table
that the optimiser does not always collapse, while a `switch` on a `uint8_t`
becomes a jump table reliably. (c) The union form is 48 bytes with the
discriminant in the first byte, which is what the tap and the recorded
command format want anyway.

**Consequence.** TBD — the `variant` comparison belongs in `bench/micro` at
M4. Until it has been run, this entry claims a design preference with
reasons, not a measured win.

---

## DD-013 — One `Event` type with a tag, not seven event structs

**Context.** Spec 9.2 sketches per-type outbound structs.

**Choice.** A single 64-byte `Event` with an `EventType` tag.

**Why.** The event stream has three consumers — the test sink, the canonical
text format used for differential testing, and the shared-memory tap — and
each of them wants a uniform record. With per-type structs, the sink
interface becomes seven functions, the text formatter becomes seven
formatters that must stay in sync with seven Python formatters, and the tap
needs a union anyway. The cost is a few unused fields per event, which is
free: the record is one cache line either way.

**Consequence.** Every event carries `counter_id`, `match_id` and `reason`
even where they are zero, and the canonical text format prints all of them.
That verbosity is deliberate: a differential diff that omits a field cannot
catch a bug in it.

---

## DD-014 — A `Fill` event as well as a `Trade` event

**Context.** One match involves two orders. The public print says what
happened; it does not say what is left on the resting order.

**Choice.** Each match emits a `Trade` (the public print: aggressor, resting,
price, quantity, aggressor's remaining) and a `Fill` for the resting order
carrying *its* remaining quantity.

**Why.** Without the `Fill`, the resting order's remaining quantity is not
recoverable from the event stream alone — a consumer would have to maintain
its own book to know it. That would make the `ShareLedger` invariant checker
dependent on a book implementation, which defeats the point of having it
re-derive share conservation independently.

**Consequence.** Two events per match rather than one. With `NullSink` in
benchmarks this costs nothing; with the tap on it doubles the tap's event
rate, which is a number that will be published alongside the tap-on/tap-off
comparison rather than hidden.

---

## DD-015 — A bounded memory of completed order ids

**Context.** Spec question 25.4: how is "already filled" distinguished from
"never existed" on a cancel, and what does that cost?

**Choice.** A FIFO ring of the last `kCompletedRingCapacity` (65536)
completed ids with an open-addressed set beside it for O(1) membership.
1.5 MB at the default size.

**Why.** Answering exactly would mean remembering every id for the whole run:
tens of millions of ids on a real day, and unbounded is not a property a
latency-sensitive system may have. Bounding it makes the memory a constant
and pushes the imprecision to a place that can be stated: **an order that
completed more than 65536 completions ago reports `UnknownOrder` rather than
`TooLateToCancel`.** At realistic completion rates that window is seconds,
which is far longer than any cancel can plausibly be in flight.

The alternative — a timestamp-based expiry — would need a clock read on the
cancel path, and the engine does not read clocks (DD-016).

---

## DD-016 — The caller assigns timestamps; the engine never reads a clock

**Context.** Spec question 25.5 asks for this to be confirmed and justified.

**Choice.** `TsNanos` arrives with every `Command` and is copied onto every
event it causes. `MatchingEngine` contains no call to any clock.

**Why.** Two reasons, both load-bearing.

*Determinism.* If the engine timestamped events itself, replaying the same
command file would produce a different event stream every time, and the
byte-identical replay guarantee — which is how `FastBook` is checked against
`NaiveBook` and how both are checked against the Python oracle — would be
impossible to state, let alone test.

*Benchmark honesty.* A clock read is 20–30 ns on most platforms. An engine
that read the clock once per command would be measuring the clock as much as
the matching, and every published per-operation number would silently include
it.

The cost is that the engine cannot enforce anything about time — it cannot
reject a stale order or expire a Day order at the close. Both belong in a
session layer above the matching core, which is where a real exchange puts
them too.

---

## DD-017 — The oracle talks to the engine only through text files

**Context.** Spec 15.1 requires the Python oracle to be independent of the
C++ implementation. "Independent" is easy to claim and easy to lose.

**Choice.** `pyref/` imports nothing from the C++ build, links against
nothing, and has no bindings. The only contact between the two
implementations is `differential.py` writing a command file, running the
compiled `engine_replay` as a subprocess, and diffing two lists of strings.
The oracle's own test suite (`pyref/tests/test_semantics.py`) passes with no
C++ build present at all; the cross-implementation tests skip.

**Why.** Bindings would create exactly the coupling the oracle exists to
avoid: a shared enum, a shared struct layout, a shared idea of what "replace"
means. The subprocess boundary costs a few milliseconds per sequence and buys
a guarantee that the only thing the two implementations share is the
specification document.

It also means the oracle is written from `docs/matching-semantics.md` rather
than from the header, which is the point of having it. Where the two
disagreed during development, the document decided which one was wrong.

---

## DD-018 — Mutation testing, because a green differential test proves nothing

**Context.** A differential test that has never failed is indistinguishable
from one that cannot fail.

**Choice.** `pyref/tests/test_differential.py` injects three known bugs into
a subclass of the reference engine — trade at the aggressor's price, size-down
loses priority, LIFO within a level — and asserts that the comparison notices
each one *and* that the shrinker reduces it to a 1-minimal reproducer.

**Consequence, and the reason this entry exists.** The "size-down loses
priority" mutant was initially **not caught**, because the random flow
generator re-randomised the price on every replace and therefore almost never
produced a replace at the order's own price. The single replace rule that
*keeps* time priority was going untested by the differential suite. The
generator now emits same-price size-downs and same-price size-ups
deliberately, at 35% and 20% of replaces.

That is the argument for mutation testing in one paragraph: the differential
suite was green, had been green for every seed tried, and had a hole in it.

---

## DD-019 — The shrinker runs to a fixpoint, not just to "smaller"

**Context.** A 1000-command failure is not a bug report.

**Choice.** Three stages: binary search for the shortest failing prefix,
delta-debugging chunk removal with halving chunk sizes, then single-command
removal repeated until a whole pass changes nothing.

**Why the fixpoint matters.** The first implementation stopped after one
single-removal pass and produced 16-command "minimal" reproducers containing
commands that were plainly irrelevant — an unknown-id cancel, orders at
prices nowhere near the failure. A reader then has to work out which of the
sixteen lines matters, which is most of the work the shrinker was supposed to
do. Removing one command can make an earlier one removable, so one pass is
not enough. The mutation tests assert 1-minimality directly: for each
remaining command, removing it must make the failure disappear.

**Cost.** More runs of the engine per failure — O(n log n) subprocess calls
rather than O(n). That is paid only when something is already broken, which
is the right place to spend time.

---

## DD-020 — Orders do not cache their price-level index

**Context.** `FastBook` finds a level by `(price - base) / tick`. The obvious
optimisation is to cache that index on the order, so cancelling saves a
subtract and a divide.

**Choice.** Do not cache it. `Order` has no `level` field; the level is
derived from the price, which the order already carries.

**Why.** The saving is one integer subtract and one shift on a path that
already does a hash lookup and touches two cache lines. What it costs is the
ability to move the ladder window cheaply.

With the index cached, re-centring the window means rewriting a field on
*every live order* — up to four million of them, and four million scattered
writes is tens of milliseconds of stall in the middle of a trading day.
Without it, re-centring touches only the occupied level records, which is
O(window) and, more importantly, independent of how many orders are resting.

So the decision is: pay one arithmetic operation per cancel, forever, to keep
the rare-but-catastrophic operation cheap. That is the right direction for a
system judged on its tail.

**Consequence.** `FastBook::recentre()` moves level records and rebuilds the
bitset; no `Order` is read or written. The
`MovesTheWindowWithoutTouchingAnyOrder` test asserts every order is still
findable at its original price afterwards.

---

## DD-021 — `PriceLevel` is padded to a cache line, and that is not obviously right

**Context.** Spec 7.3 specifies a 64-byte `PriceLevel` and then says to
measure whether the padding actually helps.

**Choice.** Padded by default, behind the `MINIEX_PAD_PRICE_LEVEL` CMake
cache variable so the comparison can actually be run.

**The argument for padding.** Two adjacent levels are frequently touched by
different logical operations, and a level record is written on every add,
fill and cancel at that price. Sharing a line means writes to the touch
invalidate the line holding the level behind it.

**The argument against, which is not weak.** Padding takes the resident
ladder from 4 MB to 16 MB per side. Only a few hundred levels are ever
occupied, so that is a great deal of cold, sparsely-touched memory, and it
spreads the *occupied* levels four times further apart — which is exactly the
wrong thing for the prefetcher when walking levels outward from the touch
during a sweep.

**Consequence.** TBD. The number that settles it is a `bench_all.sh` run with
`MINIEX_PAD_PRICE_LEVEL=ON` and `=OFF` on the Linux reference machine. Until
that has been run, this entry records a default and an argument, not a
result — and a measured negative result would be published, because an
unmeasured optimisation is worth less than a measured disappointment.

---

## DD-022 — A fixed ladder window with an overflow map, re-centred on drift

**Context.** Spec question 25.1: how wide is the ladder window, what happens
to a price outside it, and what does re-centring cost?

**Choice.**

* **Width**: `kLadderTicks` = 131072 ticks = **$13.11** at four implied
  decimals. A CMake cache variable, so the benchmark can sweep it.
* **Base**: `centre - width/2`, where `centre` is configured or, if not,
  taken from the first price the book ever sees (question 25.2). That is what
  lets the book serve an arbitrary symbol without being told its price range
  in advance.
* **Outside the window**: a per-side `std::map` overflow. Not a reject — an
  order at a legitimate price must not be refused because of an internal
  sizing choice — and not a widen, which would mean reallocating mid-session.
* **Re-centring**: when the touch enters the outermost quarter of the window,
  and at least `recentre_cooldown` operations have passed since the last one.

**Cost of re-centring.** O(window), not O(orders): occupied level records are
relocated and the bitset is rebuilt. No order record is touched (DD-020).
The cooldown exists because a book straddling an edge — bids just inside the
bottom, asks just outside the top — would otherwise re-centre on every
command; `CooldownStopsAnEdgeStraddlingBookFromThrashing` pins that down.

**How often does it fire on real data?** TBD — the honest answer needs an
ITCH day, which arrives in M6. For orientation: a $13 window centred on the
open means a stock must move more than $6.55 intraday before the touch even
approaches an edge.

**The overflow path is measured, not hidden.** It is a red-black tree and it
allocates, so it is slower than the resident path by a wide margin.
`--pattern ladder-overflow` exists to drive it hard, the differential tests
run over it, and its benchmark row sits next to the resident one in the
published results. On the development machine the overflow pattern runs at
roughly 40% of the resident pattern's throughput — slower than `NaiveBook` on
the same flow. That is a real limitation of this design and it belongs in the
results table rather than in a footnote.

---

## DD-023 — `NaiveBook` stays, forever, in CI

**Context.** Once `FastBook` exists and agrees with `NaiveBook` on millions of
sequences, `NaiveBook` is dead weight by every conventional measure: slower,
unused in production, and one more thing to keep compiling.

**Choice.** It stays, it is never optimised, and it is never removed.

**Why.** It is the only thing that can catch a bug introduced into `FastBook`
next year. The Python oracle catches the same class of bug but runs orders of
magnitude slower, so it can afford thousands of sequences where the in-repo
C++ differential affords millions. Deleting the slow, obvious implementation
after the fast one passes is deleting the reason to believe the fast one.

The corollary is a rule: **never optimise `NaiveBook`.** The moment it becomes
clever it stops being an independent check and becomes a second copy of the
same assumptions.

---

## DD-024 — `switch` dispatch in the ITCH decoder, with all three numbers published

**Context.** Spec 10.4 asks for `switch` to be measured against a 256-entry
function-pointer table and against computed goto, and for all three numbers to
be recorded.

**Measured** (development machine, under load; ratios are the usable part —
full table and caveats in `docs/itch-notes.md`):

| Strategy | messages/s | vs switch |
|---|---|---|
| Computed goto | 116.3 M | 1.18x |
| `switch` | 98.7 M | 1.00x |
| Function-pointer table | 94.4 M | 0.96x |

**Choice.** `switch`. Both alternatives stay in the tree — the table in the
header behind `DispatchMode`, computed goto in the benchmark — so the
comparison is reproducible rather than a claim.

**Why not the table.** It is slower. A dense character `switch` compiles to a
bounds check plus a jump table, and that bounds check is cheaper than an
indirect call through a pointer the branch predictor has to learn. It also
loses exhaustiveness: a `switch` missing a case is a `-Wswitch` warning and
this project builds `-Werror`; a table silently defaults.

**Why not computed goto, despite winning.** It is a GNU extension
(`&&label`, `goto *`), and the project bans extensions in public headers
(spec 3.1) because `feed/itch50.hpp` has to compile under any conforming
C++20 compiler. An 18% gain on a decoder already running at 600 MB/s — one
that is not the bottleneck in anything built on it — does not buy out that
constraint.

**A note on the measurement itself.** The computed-goto benchmark initially
looked 25% faster, because it was skipping the per-type counter bookkeeping
the real decoder does. Corrected, the gap is 18%. Two loops doing different
amounts of work are not a comparison, and the first number would have been a
flattering lie about an option that was not going to be taken anyway.

---

## DD-025 — `MarketDataBook` is a separate type, not a configuration of `FastBook`

**Context.** Both reconstruct a limit order book from add/cancel/execute
messages. Sharing one implementation is the obvious saving.

**Choice.** Two types. They share `core/` primitives and nothing else.

**Why.** They disagree about the single most load-bearing invariant in the
system. The matching engine guarantees `best_bid < best_ask` after every
operation, and `FastBook`'s correctness argument leans on it. A market data
feed does **not** guarantee it: a replace can move an order through the
opposite side, and the book stays crossed for the handful of messages until
the other side reacts.

Forcing one type to serve both means either weakening the matching book's
invariant — losing the check that catches real matching bugs — or asserting
it in the reconstructor, which then dies on real data on a normal day. That
is the classic reconstruction bug, and it is caused by exactly this
consolidation.

They also want different things from their storage.
`MarketDataBook` deliberately does **not** maintain queue position within a
level: nothing observable depends on it there, because it never matches and
neither LOBSTER's book file nor any depth feed exposes a level's queue order.
`FastBook` maintains it because there it decides who gets filled.

**Consequence.** `MarketDataBook` counts crossed and locked states rather than
asserting, and the committed test fixture contains 876 of them across 1000
messages so the tolerance is exercised rather than assumed.

---

## DD-026 — The LOBSTER fixture is generated by an independent Python reconstruction

**Context.** Real LOBSTER samples are not redistributable and are hundreds of
megabytes; the repository must still be able to test the validation path on a
machine with no market data.

**Choice.** A small synthetic but format-correct LOBSTER pair, committed
(284 KB), whose orderbook file is produced by `pyref/lobster_check.py` — a
Python reconstruction written from the LOBSTER format description, not from
`tools/lobster_check.cpp`.

**Why this shape.** The obvious alternative is to generate the expected book
with the C++ reconstruction itself, which would test that the C++ agrees with
the C++. Generating it from a separately written implementation makes the test
a genuine cross-implementation check, the same relationship the matching
engine has with `reference_engine.py`.

**What it is not.** A pass against generated data is **not** a pass against a
real LOBSTER day, and `docs/validation.md` and the README both say which one
has actually been run. Real data contains things a generator does not think to
produce: odd lots, halts, auction imbalance, LULD bands, and one venue's
sequencing quirks on one day. The generated pass is a necessary condition.

---

## DD-027 — The tap drops; it never back-pressures the engine

**Context.** The observability ring can fill. Something has to give.

**Choice.** The engine's write is one bounded store sequence. A full ring
increments `dropped` and returns immediately. The engine never waits, never
resizes, never checks whether anyone is listening.

**Why.** The alternative — slowing the matching core so a dashboard can keep
up — inverts the priorities of the entire system. The engine's latency is the
product; the dashboard is a view of it. A dashboard that can perturb what it
measures is not an instrument.

**Consequence, and it is not free.** The drop count is part of the wire
protocol and the UI renders it in red when non-zero. A silently-dropping
dashboard that looks fine is worse than no dashboard: it shows a partial
market and gives no indication that anything is missing.

**Measured.** `AReaderThatDiesMidRunDoesNotPerturbTheProducer` times the
producer with a live reader, then kills the reader and times it again over
identical work. Development machine, 200,000 records: 14069 us with a live
reader, 2426 us after the reader was killed. Losing the reader makes the
producer *faster* (the writes become early-return drops), never slower, which
is the property. The no-reader baseline is reported too but labelled as not
comparable, because with nothing draining almost every write is a drop that
never copies a record.

---

## DD-028 — The tap's cost is measured and published, not asserted

**Context.** The obvious question about a dashboard attached to a latency-
sensitive engine is whether it pollutes the numbers.

**Choice.** `engine_bench` links `TapSink` when built with `MINIEX_TAP=ON`
and `CountingSink` otherwise, and the two builds are otherwise identical. The
difference between their published numbers is the tap's cost and nothing else.

**Measured**, development machine, FastBook, 1M commands, service-time mode:

| Build | Throughput | p99 |
|---|---|---|
| tap OFF | 4.27 M cmd/s | 416 ns |
| tap ON (no reader attached) | 3.39 M cmd/s | 583 ns |

So the tap costs about 20% of throughput on this machine. That is a real cost
and it is stated rather than waved away; it is also why the tap is a
compile-time option rather than a runtime flag, and why every published
benchmark says which build produced it.

If opening the segment fails, `engine_bench` **exits** rather than running
with the tap silently off. A "tap ON" number produced with the tap not
running would be the single most misleading thing the harness could print.

---

## DD-029 — A reader that falls behind skips forward rather than lagging

**Context.** Python cannot drain a C++ matching engine running at a million
commands a second. Measured on the development machine, the reader sustains
roughly 250k events/s against 800k commands/s offered.

**Choice.** When the backlog exceeds `--max-lag`, the reader jumps its tail
forward to the newest records and counts what it skipped. The count is
published alongside the engine's own drop count.

**Why.** A reader that simply falls behind shows a *lagging replay while
claiming to be live* — the worst of both. And the lag grows without bound
until the engine's ring wraps, at which point the data is lost anyway and the
reader is showing a torn view it does not know is torn. Skipping keeps the
view current and makes the loss explicit.

**Why not make the reader faster.** It could be: a C++ reader, or numpy over
the mapped buffer, would both go considerably faster. But the coalescing means
the browser can only absorb 30 snapshots a second regardless, so a faster
reader would buy a smaller skip count and nothing a viewer could see. The
honest fix for "the dashboard cannot show ten million events a second" is to
stop pretending it can.

---

## DD-030 — The tap layout is checked across the language boundary

**Context.** `viz/server/shm_reader.py` re-declares, in `struct` format
strings, the binary layout that `viz/tap/include/viz/tap_layout.hpp` defines.

**Choice.** `tools/tap_layout_dump` prints the C++ side's constants as JSON —
including the offset of every field within a record — and
`viz/server/tests/test_layout.py` compares them field by field.

**Why this rather than generating one from the other.** Generation would be
better if the two sides were maintained separately or shipped separately.
They are not: both are built from this tree. What generation would actually
buy is a build-time dependency from Python on a code generator, in exchange
for removing a test that runs in milliseconds.

What matters is that the check exists at all, because **a layout mismatch
does not fail loudly**. The Python reader would unpack the wrong offsets and
produce plausible numbers, and the dashboard would be confidently wrong. That
is the worst possible failure mode for an observability tool, and it is worth
a dedicated test.

The magic constant is a case in point: it was first written as a hex literal
in Python with one nibble wrong, and the symptom was "not a miniex tap
segment" against a segment that plainly was one. It is now derived from the
bytes of `"MINIEXTP"`, which cannot be mistyped.

---

## DD-031 — Canvas for every panel that redraws, DOM for everything else

**Context.** The dashboard publishes at 30 Hz. React can do 30 Hz.

**Choice.** The depth ladder, the heatmap and the latency CDF are canvases.
The stats panel, the tape and the transport controls are DOM.

**Why the split is where it is.** The dividing line is not "is it a chart" but
**how much changes per frame**. The ladder's forty rows all change every
frame: reconciling forty rows of text and forty bar widths at 30 Hz is
measurable, and the flicker is visible. The stats panel changes eight numbers
once a second, and DOM is the right tool for eight numbers with labels and
semantics.

The tape is DOM despite being the busiest panel, because it is a *list* and
lists want virtualisation, selection and scrolling — all of which the browser
already does well and none of which a canvas gives for free.

**Consequence.** The canvases need their own tests, because there is no DOM to
assert against. `src/test-setup.ts` stubs `getContext` with a *recording*
context, so a test can assert that a panel actually issued draw calls rather
than merely mounting.

---

## DD-032 — The heatmap scrolls an offscreen canvas; it does not redraw history

**Context.** 900 columns of 30-second history at up to 5000 price rows.

**Choice.** Each publish tick draws **one** new column into an offscreen
canvas, `drawImage`s that canvas onto itself shifted left by one column, and
the visible canvas is a single blit. The whole buffer is only re-rendered when
the price window moves.

**Why.** Redrawing everything would be 900 × 600 cells per frame at 30 Hz —
16 million cells a frame, 486 million a second. No browser will do that, and
the panel that is supposed to be the striking one would be the one that
stutters.

**The supporting decision is the window hysteresis.** The visible price range
only moves when the touch leaves the middle half of it. Recomputing it from
the current book every frame would make the whole heatmap slide vertically
whenever one deep order arrives, and a chart whose axis moves is unreadable —
and it would also force a full 900-column redraw on every frame, which is the
thing this design exists to avoid. The two are the same decision.

---

## DD-033 — The UI is verified by jsdom, not by screenshots

**Context.** The spec asks for a committed screenshot and a demo GIF.

**Choice.** The dashboard is tested by mounting it in jsdom against a stubbed
WebSocket and feeding it the committed sample recording — the same file, byte
for byte, that the mock server replays. The tests assert that the canvases
issue draw calls, the tape renders newest-first, transport commands reach the
socket, and **zero frames are rejected**.

**Why not screenshots.** A screenshot proves the UI looked right once, on one
machine, and then rots silently. It also cannot run in CI. The jsdom tests run
on every push and fail when the protocol drifts, which is the failure that
actually happens.

**What is genuinely missing.** The screenshot and the GIF. They are not
committed because this environment could not drive a browser: the available
Chrome window was occluded, which freezes the page's timers, and every capture
timed out against a renderer that was alive but frozen. `viz/ui/README.md`
says so and gives the commands to produce them, rather than shipping a
mock-up.

---

## DD-034 — Pre-fault the pool, the index and the ladder at construction

**Context.** `FastBook` at 262144 orders allocates roughly 40 MB across its
slab, its hash table and its price ladder. `operator new` returns virtual
addresses; the physical pages behind them do not exist until something writes
to each one, and each of those first writes is a page fault.

**What made it visible.** The open-loop harness reported `FastBook`'s p99 as
*worse* than `NaiveBook`'s, which contradicted every microbenchmark: in
service time (dequeue to handler return) `FastBook` was better (375 ns against
500 ns at p99), and in the mixed-flow microbenchmark it was 1.68× faster.

The difference between those two measurements is what they see. A
microbenchmark faults its whole working set in during the warm-up iterations
Google Benchmark discards. A throughput number amortises ten thousand faults
over a million commands. An **open-loop tail-latency measurement sees every
one of them**, scattered through p99 and p99.9, because a page fault is a trap
into the kernel and a TLB shootdown — microseconds, landing on whichever
unlucky command triggered it.

`NaiveBook` pays a smaller version of the same cost, spread over many small
allocations, so the artefact made the slower book look better.

**Choice.** `core/prefault.hpp` writes one byte per 4 KB page, and
`ObjectPool`, `OpenHashMap` and `FastBook`'s ladder all call it in their
constructors. Construction is allowed to be slow; steady state is not, and
this is the standard thing a latency-sensitive system does at start-up.

A **write**, not a read: reading an untouched anonymous page may be served
from the shared zero page, deferring the real allocation to the first write —
exactly the moment this exists to avoid.

**Consequence — and the honest caveat.** After the change, `FastBook`'s
open-loop p99.9 on this machine came out at 106 µs against 730 µs before, and
its p99 at 14.6 µs against 34.8 µs. Those look like a large win and they
probably are, but **this development machine cannot support the claim**: its
load average during these runs was between 34 and 124 on ten cores, with
Google Drive alone taking a full core. One scheduler preemption is tens of
microseconds and lands straight in p99.9, so the tails here describe the
machine at least as much as the engine.

The change is kept because the reasoning is sound and the cost is a few
milliseconds at start-up, not because the measurement settles it. It is
recorded here as *suggestive*, and `scripts/machine_report.sh` now captures
the load average and the top CPU consumers so that a future reader can see
immediately whether a result set was taken on a quiet machine. This one was
not.
