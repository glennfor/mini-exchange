# Divergence: what happens when ITCH order flow meets our engine

Read this before quoting any number from `tools/inject`.

---

## 1. The conceptual point

**ITCH is a market data feed. It is not an order entry stream.** It reports
what Nasdaq already matched, after the fact, as a sequence of statements about
a book that has already changed.

You therefore cannot "replay ITCH through a matching engine" and expect
identical fills, and a project that claims to is either confused or being
dishonest. There is no order entry stream to replay. The exchange's matching
decisions were made against information — hidden orders, reserve quantity, the
precise arrival sequence inside a microsecond — that the feed does not contain
and never will.

What can honestly be done, and what `tools/inject` does:

* take the `A`/`F` add messages as an **order arrival process**;
* submit them to our engine as fresh limit orders;
* apply `X`/`D`/`U` by reference, so our resting population tracks the real one;
* compare the trades **our** engine generates against the `E`/`C`/`P`
  executions in the feed;
* **characterise and explain every difference.**

The last step is the deliverable. A divergence you can name and quantify is a
finding. An unexplained one is a bug.

## 2. The message mapping, and where it is not one-to-one

| ITCH | Injected as | Why not the obvious thing |
|---|---|---|
| `A` / `F` | `NEW` limit/day, id = the ITCH order reference | Using the reference as the engine's order id means every later message lines up with no translation table |
| `X` | `REPLACE` to `open − cancelled` at the same price | ITCH cancels **reduce**; our replace takes the new **absolute** open quantity. The injector must ask the engine for the current one — and for the price, because the cancel message carries none |
| `D` | `CANCEL` | |
| `U` | `CANCEL old` then `NEW new` | **Not** our engine's `REPLACE`. ITCH's replace introduces a *new reference number* and goes to the back of the queue; our `REPLACE` keeps the client's id. Using it would leave the old reference alive, and every later message for the new one would be an unknown order |
| `E` / `C` | **not injected** | these are the exchange's fills — the thing being compared against |
| `P` | **not injected** | matched against liquidity we never saw |
| `Q` | **not injected** | an auction our engine does not implement |

A subtlety worth stating: when our engine has already traded an order out and
the feed then sends a cancel for it, the injector emits nothing. The divergence
is real but it is already counted once, on the execution side; counting it again
as a failed cancel would double-count the same disagreement.

## 3. Expected divergence classes

These are structural. They are consequences of ITCH being a market data feed
and of this engine's stated scope, not defects.

### 3.1 Hidden and reserve orders

Nasdaq matches against liquidity the feed never describes. A `P` message is a
non-displayed execution: a trade with no corresponding resting order in any
reconstruction, because the order was never published. Our engine will miss
every fill the real book made against hidden size, and will instead fill the
displayed order behind it — or not fill at all.

This is the largest single class on a liquid name. It is measured directly:
`P` volume is reported as its own line in the divergence table.

### 3.2 Auction crosses

`Q` messages move enormous volume at the open and the close through a call
auction. Auction matching is explicitly out of scope (spec 2.2), so our engine
produces none of it. Cross volume is reported as its own line, and on a normal
day it is a large fraction of total volume concentrated into two moments.

### 3.3 Odd lots and round-lot conventions

Odd-lot orders and odd-lot portions of executions follow different display and
reporting rules. Our engine treats every quantity identically, so its book
contains displayed size the real book did not show, and vice versa.

### 3.4 Order arrival versus execution ordering

ITCH timestamps are **publication** times, not arrival times. Two orders
published in one order may have arrived in the other; the feed cannot tell you.
Our engine imposes its own acceptance order — the order the messages appear in
the file — and price-time priority then hands the fill to a different order
than the exchange did. The trade still happens, at the same price, for the same
quantity; a *different order* gets it.

This is why the accounting is done in **shares against a named resting order**
rather than in trade events. An event-level comparison would report a
divergence every time priority resolved differently, which is neither
interesting nor avoidable.

### 3.5 Routed-away liquidity

Nasdaq is one venue. An order that Nasdaq could not fill may have been routed
elsewhere and filled there, and the feed shows it leaving the book without a
Nasdaq execution. Our engine, which is the whole market as far as it knows,
will either fill it or leave it resting.

### 3.6 The window boundary

A run over part of a day sees executions and cancels for orders added before
the first message read. These are counted as **"reference never injected"** and
kept out of the unexplained bucket, because attributing them to a modelling
failure would be wrong. A run from the opening cross should show zero.

## 4. The results table

| Metric | Value |
|---|---|
| Add messages injected | TBD |
| Trades generated by engine | TBD |
| Executions in the feed | TBD |
| Matched (same resting order, same price) | TBD |
| Explained by hidden liquidity (`P`) | TBD |
| Explained by cross/auction (`Q`) | TBD |
| Unexplained | TBD |

**These are TBD because no real ITCH sample day has been downloaded.**
`tools/inject` is implemented, tested, and runs; what it has been run against
so far is a synthetic file, and the numbers from that file are meaningless for
this purpose — the synthetic generator emits adds and executions independently,
with no matching logic connecting them, so it is not internally consistent in
the way a real feed is. Publishing its 62.8% "unexplained" figure as though it
said something about this engine would be worse than publishing nothing.

To fill the table in:

```bash
./scripts/fetch_data.sh itch
gunzip -k data/nasdaq_itch50.gz
./build/release/tools/itch_stats --file data/nasdaq_itch50   # find the locate
./build/release/tools/inject --file data/nasdaq_itch50 --locate <n> --cases 20
```

The **unexplained** row must be small, and every case in it must have been
looked at individually. If it is 30%, that is a bug, not a finding.

## 5. Individual cases

Spec 13 requires at least three individual divergences written up with message
indices. `tools/inject --cases N` prints them — message index, timestamp, order
reference, the feed's quantity and price, and which bucket the case fell into.

**Not yet written up, for the same reason as §4.** They will be written against
real data or not at all; three invented examples would be worse than an empty
section.

## 6. What the flow profile is for

`tools/flow_profile` measures, from a real file:

* the **message** mix (what ITCH sent) and the derived **command** mix (what an
  order gateway would have had to send to produce it). These are not the same
  thing, and conflating them is the usual mistake: executions are not commands
  — nobody sends them, they are the engine's output — and an ITCH `X` is a
  reduction while a `D` is a removal, so they map to different commands;
* the order-lifetime distribution, in both nanoseconds and intervening
  messages, which is what makes cancels concentrate on recent orders;
* the fraction of adds that ever trade at all;
* the depth at which orders are placed relative to the touch.

Its output is a ready-made `engine_bench --mix` line. Once it has been run on
real data, the benchmark's synthetic default is replaced by the measured mix
and **both** are published, because the difference between them is itself the
interesting number.
