# Matching Semantics

This is the behavioural contract of `engine/matching_engine.hpp`. It is also
the document the Python reference oracle in `pyref/` is written from —
deliberately, so the oracle is independent of the C++ source rather than a
transcription of it.

Every rule below has a test named after it. Where a rule is subtle, the test
name says which one it is defending.

---

## 1. Price-time priority

1. A better price always executes first: higher for a bid, lower for an ask.
2. Within a price level, strict FIFO by **engine acceptance order** — not by
   the client's timestamp, which the engine does not trust and does not use
   for ordering.
3. An incoming aggressive order walks the opposing side from the touch
   outwards, consuming each level's FIFO head first, until it is filled, its
   limit price is exceeded, or the opposing side is exhausted.

> Tests: `BetterPriceExecutesFirst`,
> `WithinAPriceLevelStrictFifoByArrival`,
> `AggressorWalksLevelsUntilItsLimitIsExceeded`.

## 2. Trade price

**A trade executes at the resting order's price, never the incoming order's.**

An aggressive buy at 101 hitting a resting ask at 100 prints at 100. The
resting order set the terms by being there first; the aggressor's willingness
to pay more is not the market's business, and the price improvement accrues
to the aggressor.

> Tests: `TradeExecutesAtTheRestingOrdersPriceNotTheIncomingOrders`,
> `TradeAtRestingPriceAlsoWhenTheRestingSideIsTheBid`. The `ShareLedger`
> invariant checker also re-derives this from the event stream alone.

## 3. Order types and time in force

| Type / TIF | Behaviour | Reject reason if refused |
|---|---|---|
| Limit + Day | Match what it can; the residual rests. | — |
| Limit + IOC | Match what it can; the residual is cancelled, not rested. | — |
| Limit + FOK | Pre-check the fillable quantity within the limit. If it cannot fill **entirely**, execute nothing and reject. | `FokUnfillable` |
| Limit + PostOnly | If it would cross on arrival, reject it entirely. The price is **not** slid. | `PostOnlyWouldCross` |
| Market + IOC | A limit at ±∞ with IOC semantics. | — |
| Market + Day | **Rejected.** | `MarketDayNotSupported` |
| Market + FOK | Fill entirely at any price, or reject. | `MarketOrderNotFillable` |
| Market + PostOnly | **Rejected.** A market order that must not cross can never do anything. | `MarketDayNotSupported` |

Notes:

* **Why PostOnly rejects rather than slides.** A price slide silently gives
  the client a different order from the one they sent. They then have to
  reconcile their own intent against the exchange's reinterpretation, forever.
  Rejecting is louder and cheaper for everyone.
* **The FOK pre-scan** (`MatchingEngine::fillable_qty`) is `const` and is the
  one place where matching is two-pass. It terminates as soon as the
  accumulated quantity reaches what is wanted, and is additionally capped at
  `EngineConfig::max_fok_scan_levels` levels so a pathologically deep book
  cannot turn it into an unbounded walk on the hot path.
* **With self-trade prevention on**, the pre-scan excludes resting orders
  belonging to the aggressor's own participant, because STP will cancel those
  rather than trade with them. Counting them would make FOK a lie: the engine
  would accept an order it then could not fill.

## 4. Cancel

* Cancel by id.
* Unknown id → `UnknownOrder`.
* Order already filled or already cancelled → `TooLateToCancel`.
* The `Cancelled` event's `qty` is the quantity that was **still open**, not
  the original quantity.

Telling "already gone" apart from "never existed" needs memory of completed
ids. That memory is a bounded FIFO of `EngineConfig::completed_capacity`
entries (65536 by default) — see `engine/completed_ids.hpp`. **At the
boundary: an order that completed more than that many completions ago reports
`UnknownOrder`.** That is a documented consequence of bounding the memory,
not an accident, and the Python oracle implements the same bound so the two
agree there too.

> Tests: `CancelOfAFullyFilledOrderIsTooLateNotUnknown`,
> `CompletedIdMemoryIsBoundedAndSaysSoAtTheBoundary`.

## 5. Cancel / replace

`ReplaceOrder::new_qty` is the new **open** quantity — not a delta, and not a
new original quantity. This matches ITCH's Order Replace message.

| Change | Priority |
|---|---|
| Quantity decrease at the same price | **Keeps** time priority. |
| Quantity increase | **Loses** time priority; moves to the level tail. |
| Price change, either direction | **Loses** time priority; requeued at the new level's tail. |
| Price change that crosses the book | Matches immediately as an aggressive order; any residual rests at the new price. |

A size-down keeps priority because the client is asking for strictly less —
nobody queued behind them is disadvantaged by letting them stay. Every other
change asks for something more than they had, and taking it without going to
the back of the queue would be taking it from the people behind them.

**Replace is atomic.** The capacity check happens *before* the original order
is removed, so a replace that cannot be completed leaves the book exactly as
it was and returns `PoolExhausted`. A cancel-then-add implementation cannot
promise this: if the add fails, the client's order has silently vanished.

Event ordering for a replace: `Replaced` first, then any trades it causes.

> Tests: the whole of `engine/tests/replace_test.cpp`, one per row, plus
> `ReplaceIntoAnExhaustedPoolLeavesTheOriginalUntouched`.

## 6. Self-trade prevention

Off by default; enabled by `EngineConfig::stp`.

**Implemented: `CancelResting`.** When an aggressor would match a resting
order carrying the same `participant_id`, the *resting* order is cancelled
(with `reason = SelfTradePrevented`) and the aggressor continues walking the
book.

Participant `0` means "unattributed" and never self-trade-prevents; without
that carve-out every default-constructed order would prevent against every
other one.

The alternatives, not implemented:

* **CancelIncoming** — cancel the aggressor instead, leaving the book intact.
  Kinder to the resting queue, but it lets a passive order block its owner's
  ability to trade with anyone, because the aggressor dies at the first
  self-match instead of walking past it.
* **CancelBoth** — remove both sides. Symmetric and simple, but it destroys
  liquidity the participant may have wanted to keep, and it makes the cost of
  an accidental self-cross twice as large as it needs to be.
* **DecrementAndCancel** — reduce the larger order by the smaller and cancel
  the smaller. What most US equity venues actually do, because it preserves
  as much of the participant's intent as possible; it is also the fiddliest
  to get right, since the decrement must not change the surviving order's
  priority.

Note on naming: the spec calls this policy "CancelNewest" and then describes
cancelling the resting order. Since the resting order is by construction the
*older* of the two, the behaviour described is what the industry usually
calls CancelOldest. The implementation follows the described behaviour, and
the enumerator is named `CancelResting` to say plainly which order dies.

## 7. Rejection reasons

Exhaustive. Every reject path in the engine maps to exactly one:

`None`, `DuplicateOrderId`, `UnknownOrder`, `TooLateToCancel`,
`ZeroQuantity`, `InvalidPrice`, `PriceOutOfBand`, `PoolExhausted`,
`FokUnfillable`, `PostOnlyWouldCross`, `MarketOrderNotFillable`,
`MarketDayNotSupported`, `SelfTradePrevented`.

`RejectReason` also appears on two non-reject events, where it explains a
cancellation the client did not ask for:

* `Cancelled` with `SelfTradePrevented` — STP removed a resting order.
* `Cancelled` with `PoolExhausted` — an order matched, but there was no
  storage left for its residual. Distinguishing this from an ordinary IOC
  cancellation matters: one is what the client asked for, the other is the
  exchange running out of room.

## 8. Invariants

Checked after every operation by the property suite
(`engine/tests/invariant_test.cpp`). Items 1 and 3–5 are checked against the
book's own state by `check_book()`; items 2, 6 and 7 are re-derived from the
event stream alone by `ShareLedger`, so two independent things have to agree.

1. `best_bid < best_ask` whenever both sides are non-empty. The book is never
   crossed or locked after an operation completes.
2. Share conservation: `qty_orig == qty_filled + qty_open + qty_cancelled`
   for every order ever accepted.
3. A level's aggregate quantity equals the sum of its FIFO members' open
   quantities.
4. The id index size equals the sum of the level order counts.
5. Within a level, entry sequence is non-decreasing from head to tail.
6. No trade price lies outside the range spanned by the two orders' limits,
   and every trade prints exactly at the resting order's price.
7. Total traded quantity on the buy side equals the total on the sell side.
8. `pool.allocated + pool.free == pool.capacity` (`FastBook` only).

**Invariant 1 applies to the matching engine only.** `MarketDataBook` (feed
reconstruction) must tolerate transiently locked and crossed states, because
real exchange feeds produce them. Asserting no-cross there is the classic
reconstruction bug; see `docs/validation.md`.

## 9. Determinism

Given the same command stream and the same build, the engine produces a
byte-identical event stream. This is guaranteed by:

* no wall-clock reads — the timestamp arrives **with** the command and the
  engine never calls a clock (`EngineNeverReadsTheClock`);
* no RNG, no hash-order iteration in anything that emits;
* every outbound event carrying a monotonic `SeqNum` with no gaps
  (`EverySequenceNumberIsUsedExactlyOnceInOrder`).

Verified by `engine_replay --record` / `--verify`, run across two invocations
and across optimisation levels.
