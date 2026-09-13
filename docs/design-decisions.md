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
