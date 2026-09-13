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
