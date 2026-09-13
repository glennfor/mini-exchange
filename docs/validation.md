# Validation

How the reconstruction is checked, what has actually been run, and what has
not.

---

## 1. What is being validated

`MarketDataBook` reconstructs the exchange's own book from the message stream.
Its correctness claim is: **after every single message, the top N levels on
both sides match the book the exchange published.** Not "at the close", not
"every hundredth message" — after every message. A reconstruction can be wrong
for ten thousand messages and right again by the time anyone looks, and a
coarse comparison will not notice.

LOBSTER is the reference because it publishes exactly the artefact this needs:
a message file and an orderbook file **in lockstep, one book row per message**.

## 2. The two ways `MarketDataBook` differs from the matching book

These are the distinctions that make reconstruction its own problem rather
than a use of `FastBook`.

### The no-crossed-book invariant does not apply

Spec 8.8 invariant 1 — `best_bid < best_ask` — is a property of the **matching
engine**. It is not a property of a market data feed.

Real feeds produce transiently locked (`bid == ask`) and crossed
(`bid > ask`) books. An ITCH `U` replace can move an order to a price through
the opposite side, and the state persists for the handful of messages until
the other side reacts. Odd lots, non-displayed liquidity and the sequencing of
independent message streams all produce it too.

A reconstructor that asserts no-cross dies on real data on a normal day. This
one counts crossed states and reports them, and the count being zero over a
full day would itself be suspicious. The committed test fixture deliberately
contains 876 crossed states across 1000 messages so that the tolerance is
exercised rather than assumed.

### Unknown references are expected, not errors

An execution, cancel or delete for a reference number never seen is normal
whenever a file is joined mid-day — which is every sample that does not begin
at the opening cross. They are counted and reported. `--strict` turns them
into errors, for a run known to start at the beginning of the session.

## 3. The LOBSTER direction trap

> For **execution** messages (types 4 and 5) the direction field describes the
> side of the **limit order that was executed**, not the aggressor.

A sell market order lifting a bid appears with direction `1`, because the
order that was executed was a bid.

This is the single most common LOBSTER bug, and what makes it nasty is that it
does **not** break the book reconstruction — the reconstruction only needs to
know which side the referenced order is on, which is exactly what the field
says. It breaks anything that infers trade *sign*: order-flow imbalance, trade
classification, signed volume. The mistake therefore survives every
book-level validation and shows up much later as a signal with the wrong sign.

`LobsterMessage::aggressor_side()` exists so the inversion is written down
once instead of being re-derived, and re-mistaken, at every call site. It has
its own test, named for the trap.

Other things worth knowing about the format:

* **Time** is decimal seconds after midnight. It is parsed as two integer
  fields split on the decimal point, never through a `double`: a `double`
  holds about 15 significant digits and `34200.123456789` needs 14, so parsing
  through one is close enough to look right and close enough to be wrong in
  the last place.
* **Price** is dollars × 10000, which is this project's `Price` scale exactly.
  No conversion, no rounding.
* **Absent levels** are padded with a sentinel price (`±9999999999`) and size
  0. Without recognising the sentinel, "there is no fifth level" reads as
  "there is a level at $999,999.9999".
* **Hidden executions (type 5) do not change the visible book.** They matched
  liquidity the visible book never had.

## 4. What has actually been run

Being precise about this, because "validated against LOBSTER" is a claim
people make loosely.

| Check | Status |
|---|---|
| Reconstruction vs an independently-written Python reconstruction, 1000 messages, 10 levels, after every message | **passing** |
| Same, with 876 crossed/locked states in the fixture | **passing** |
| Reconstruction vs a **real LOBSTER sample day** | **not yet run** — no sample day has been downloaded |
| ITCH ↔ LOBSTER cross-check | **not yet run** — see §6 |

### What the passing check does and does not establish

The committed fixture (`replay/tests/data/`) is a synthetic but
format-correct LOBSTER pair. Its orderbook file was produced by
`pyref/lobster_check.py`, a Python reconstruction written from the format
description rather than from the C++ source. So the passing test is a genuine
cross-implementation check, the same shape as the matching engine's three-way
agreement: two reconstructions, written separately, compared message by
message at ten levels.

What it establishes is that the **machinery** is right — the parser, the
message table, the aggregation, the comparison, the crossed-book tolerance.

What it does **not** establish is agreement with a real exchange. Real data
contains things a generator does not think to produce: odd lots, halts,
auction imbalance messages, LULD bands, reference numbers that wrap
expectations, and the specific sequencing quirks of one venue on one day. A
pass here is a necessary condition, not the claim.

Reproduce it with:

```bash
# regenerate the fixture (deterministic; --seed 7)
python -m pyref.lobster_check generate \
    --out-prefix replay/tests/data/SYNTH_2026-09-13_34200_57600 \
    --seed 7 --count 1000 --levels 10

# the Python side
python -m pyref.lobster_check validate \
    --messages replay/tests/data/SYNTH_2026-09-13_34200_57600_message_10.csv \
    --book     replay/tests/data/SYNTH_2026-09-13_34200_57600_orderbook_10.csv

# the C++ side
./build/release/tools/lobster_check \
    --messages replay/tests/data/SYNTH_2026-09-13_34200_57600_message_10.csv \
    --book     replay/tests/data/SYNTH_2026-09-13_34200_57600_orderbook_10.csv \
    --levels 10
```

### How to run it against real data

```bash
./scripts/fetch_data.sh lobster
./build/release/tools/lobster_check \
    --messages data/lobster/<SYMBOL>_<DATE>_34200000_57600000_message_10.csv \
    --book     data/lobster/<SYMBOL>_<DATE>_34200000_57600000_orderbook_10.csv \
    --levels 10
```

Success is the line `VALIDATION PASSED: N messages, N book rows compared at 10
levels per side, zero mismatches`. On a mismatch the tool prints the message
index, the message, both books' top ten, and marks the first differing level.

When that has been run, this table and the README's validation section will
say so with the symbol, the date, and the message count. Until then they say
it has not been.

## 5. Reading a mismatch report

```text
LOBSTER MISMATCH
  message index: 41882
  message:       type=VisibleExecution(4) id=11885113 size=100 price=2238100 side=B ts=...
  first differs: bid side, level 1 (size)

  reconstructed                      | LOBSTER
  price            size              | price            size
  -- asks --
    2238200          300             | 2238200          300
  -- bids --
  > 2238100          400             | 2238100          500
```

The usual causes, in the order worth checking:

1. **A hidden execution treated as a book change.** Type 5 does not touch the
   visible book. This shows up as a level that is too small.
2. **A partial cancel treated as a deletion.** Type 2 reduces; type 3 removes.
3. **A replace reusing the old reference number.** The new order is a
   different order, at the tail of its new level, with a new reference.
4. **The file starting mid-day.** Check the unknown-reference count first; if
   it is non-zero the book was already missing orders before the first
   mismatch.

## 6. ITCH ↔ LOBSTER cross-check

If an ITCH sample day and a LOBSTER sample day cover the same symbol and date,
reconstructing from ITCH and diffing against the LOBSTER book file is a
stronger check than either alone, because the two paths share nothing but the
underlying market.

**This has not been done, because the dates do not overlap.** The freely
available LOBSTER samples are from 2012; the freely available Nasdaq ITCH
samples are from later years. Saying so plainly is better than implying a
cross-validation that has not happened.

Where the two do differ on an overlapping day, the cause is almost always one
of: odd-lot handling, non-displayed orders, or exchange-versus-consolidated
scope. Each would be written up here with a concrete message index.
