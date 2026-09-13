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
